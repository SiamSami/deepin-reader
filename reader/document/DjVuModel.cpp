// Copyright (C) 2019 ~ 2020 Uniontech Software Technology Co.,Ltd.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DjVuModel.h"
#include "Application.h"
#include "ddlog.h"

#include <QFile>
#include <QUuid>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <qmath.h>
#include <cstdio>
#include <unistd.h>

#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>

#define LOCK_DOC QMutexLocker locker(&m_mutex);

namespace {

inline miniexp_t skip(miniexp_t exp, int offset)
{
    while (offset-- > 0)
        exp = miniexp_cdr(exp);
    return exp;
}

void clearQueue(ddjvu_context_t *ctx, bool wait)
{
    if (wait)
        ddjvu_message_wait(ctx);

    while (ddjvu_message_peek(ctx))
        ddjvu_message_pop(ctx);
}

void waitForTag(ddjvu_context_t *ctx, ddjvu_message_tag_t tag)
{
    ddjvu_message_wait(ctx);

    while (true) {
        auto *msg = ddjvu_message_peek(ctx);
        if (!msg) break;
        if (msg->m_any.tag == tag) break;
        ddjvu_message_pop(ctx);
    }
}

QString extractText(miniexp_t exp, QSizeF size, const QRectF &rect)
{
    if (miniexp_length(exp) < 6 || !miniexp_symbolp(miniexp_car(exp)))
        return {};

    int xmin = miniexp_to_int(miniexp_cadr(exp));
    int ymin = miniexp_to_int(miniexp_caddr(exp));
    int xmax = miniexp_to_int(miniexp_cadddr(exp));
    int ymax = miniexp_to_int(miniexp_caddddr(exp));

    QRectF box(xmin, size.height() - ymax, xmax - xmin, ymax - ymin);
    if (!rect.intersects(box))
        return {};

    QString type = QString::fromUtf8(miniexp_to_name(miniexp_car(exp)));

    if (type == "word")
        return QString::fromUtf8(miniexp_to_str(miniexp_nth(5, exp)));

    QStringList result;
    exp = skip(exp, 5);

    for (; miniexp_consp(exp); exp = miniexp_cdr(exp))
        result << extractText(miniexp_car(exp), size, rect);

    return type == "line" ? result.join(" ") : result.join("\n");
}

} // namespace

namespace deepin_reader {

// ===================== DjVuPage ======================

DjVuPage::DjVuPage(const DjVuDocument *parent, int index, const ddjvu_pageinfo_t &info)
    : m_parent(parent),
      m_index(index),
      m_size(info.width, info.height),
      m_resolution(info.dpi)
{}

QSizeF DjVuPage::sizeF() const
{
    return m_size;
}

QImage DjVuPage::render(int w, int h, const QRect &slice) const
{
    LOCK_DOC

    auto *page = m_parent->getPage(m_index);
    if (!page) return {};

    ddjvu_rect_t pagerect{0,0,(unsigned)w,(unsigned)h};
    ddjvu_rect_t renderrect;

    if (slice.isNull())
        renderrect = pagerect;
    else {
        renderrect.x = slice.x();
        renderrect.y = slice.y();
        renderrect.w = slice.width();
        renderrect.h = slice.height();
    }

    QImage img(renderrect.w, renderrect.h, QImage::Format_RGB32);

    if (!ddjvu_page_render(page,
                           DDJVU_RENDER_COLOR,
                           &pagerect,
                           &renderrect,
                           m_parent->m_format,
                           img.bytesPerLine(),
                           (char*)img.bits()))
        return {};

    return img;
}

QString DjVuPage::text(const QRectF &rect) const
{
    LOCK_DOC

    auto exp = m_parent->getPageText(m_index);
    if (!exp) return {};

    QTransform scale = QTransform::fromScale(m_resolution/72.0, m_resolution/72.0);

    return extractText(exp, m_size, scale.mapRect(rect)).simplified();
}

QVector<PageSection> DjVuPage::search(const QString &text,
                                      bool matchCase,
                                      bool wholeWords) const
{
    LOCK_DOC

    auto exp = m_parent->getPageText(m_index);
    if (!exp) return {};

    QVector<PageSection> results;

    QStringList words = text.split(QRegularExpression("\\W+"), Qt::SkipEmptyParts);
    if (words.isEmpty()) return results;

    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

    QVector<miniexp_t> queue{exp};

    while (!queue.isEmpty()) {
        auto node = queue.takeFirst();

        if (miniexp_length(node) < 6 || !miniexp_symbolp(miniexp_car(node)))
            continue;

        QString type = QString::fromUtf8(miniexp_to_name(miniexp_car(node)));

        if (type == "word") {
            QString word = QString::fromUtf8(miniexp_to_str(miniexp_nth(5, node)));

            for (auto &w : words) {
                if (word.contains(w, cs)) {
                    int xmin = miniexp_to_int(miniexp_cadr(node));
                    int ymin = miniexp_to_int(miniexp_caddr(node));
                    int xmax = miniexp_to_int(miniexp_cadddr(node));
                    int ymax = miniexp_to_int(miniexp_caddddr(node));

                    QRectF r(xmin, m_size.height()-ymax, xmax-xmin, ymax-ymin);
                    results << PageSection{PageLine{word,r}};
                }
            }
        } else {
            node = skip(node,5);
            for (; miniexp_consp(node); node=miniexp_cdr(node))
                queue << miniexp_car(node);
        }
    }

    return results;
}

// ===================== DjVuDocument ======================

DjVuDocument::DjVuDocument(ddjvu_context_t *ctx, ddjvu_document_t *doc)
    : m_context(ctx),
      m_document(doc)
{
    unsigned int mask[] = {0x00ff0000,0x0000ff00,0x000000ff,0xff000000};
    m_format = ddjvu_format_create(DDJVU_FORMAT_RGBMASK32,4,mask);
    ddjvu_format_set_row_order(m_format,1);
    ddjvu_format_set_y_direction(m_format,1);

    prepareFileInfo();
}

DjVuDocument::~DjVuDocument()
{
    qDeleteAll(m_pages);

    for (auto p : m_pageCache)
        ddjvu_page_release(p);

    for (auto e : m_textCache)
        ddjvu_miniexp_release(m_document,e);

    ddjvu_format_release(m_format);
    ddjvu_document_release(m_document);
    ddjvu_context_release(m_context);
}

// ---------- caching helpers ----------

ddjvu_page_t* DjVuDocument::getPage(int index) const
{
    if (m_pageCache.contains(index))
        return m_pageCache[index];

    auto *page = ddjvu_page_create_by_pageno(m_document,index);
    if (!page) return nullptr;

    while (ddjvu_page_decoding_status(page) < DDJVU_JOB_OK)
        clearQueue(m_context,true);

    if (ddjvu_page_decoding_status(page) >= DDJVU_JOB_FAILED) {
        ddjvu_page_release(page);
        return nullptr;
    }

    m_pageCache[index] = page;
    return page;
}

miniexp_t DjVuDocument::getPageText(int index) const
{
    if (m_textCache.contains(index))
        return m_textCache[index];

    miniexp_t exp;
    while (true) {
        exp = ddjvu_document_get_pagetext(m_document,index,"word");
        if (exp != miniexp_dummy) break;
        clearQueue(m_context,true);
    }

    m_textCache[index] = exp;
    return exp;
}

// ---------- document loading ----------

DjVuDocument* DjVuDocument::loadDocument(const QString &path, Document::Error &err)
{
    auto ctx = ddjvu_context_create("deepin_reader");
    if (!ctx) { err=Document::FileError; return nullptr; }

    auto doc = ddjvu_document_create_by_filename_utf8(ctx,path.toUtf8(),FALSE);
    if (!doc) {
        ddjvu_context_release(ctx);
        err=Document::FileError;
        return nullptr;
    }

    waitForTag(ctx,DDJVU_DOCINFO);

    if (ddjvu_document_decoding_error(doc)) {
        ddjvu_document_release(doc);
        ddjvu_context_release(ctx);
        err=Document::FileError;
        return nullptr;
    }

    err=Document::NoError;
    return new DjVuDocument(ctx,doc);
}

// ---------- document API ----------

int DjVuDocument::pageCount() const
{
    LOCK_DOC
    return ddjvu_document_get_pagenum(m_document);
}

Page* DjVuDocument::page(int index) const
{
    LOCK_DOC

    ddjvu_pageinfo_t info;
    while (ddjvu_document_get_pageinfo(m_document,index,&info) < DDJVU_JOB_OK)
        clearQueue(m_context,true);

    auto *p = new DjVuPage(this,index,info);
    m_pages << p;
    return p;
}

bool DjVuDocument::saveAs(const QString &path) const
{
    LOCK_DOC

    FILE *file = fopen(QFile::encodeName(path),"wb");
    if (!file) return false;

    auto job = ddjvu_document_save(m_document,file,0,nullptr);

    while (!ddjvu_job_done(job))
        clearQueue(m_context,true);

    fclose(file);

    return !ddjvu_job_error(job);
}

Properties DjVuDocument::properties() const
{
    LOCK_DOC

    Properties props;
    miniexp_t exp;

    while (true) {
        exp = ddjvu_document_get_anno(m_document,TRUE);
        if (exp != miniexp_dummy) break;
        clearQueue(m_context,true);
    }

    for (; miniexp_consp(exp); exp=miniexp_cdr(exp)) {
        auto item = miniexp_car(exp);
        if (miniexp_length(item) != 2) continue;

        props.insert(QString::fromUtf8(miniexp_to_name(miniexp_car(item))),
                     QString::fromUtf8(miniexp_to_str(miniexp_cadr(item))));
    }

    ddjvu_miniexp_release(m_document,exp);
    return props;
}

// ---------- file info ----------

void DjVuDocument::prepareFileInfo()
{
    for (int i=0;i<ddjvu_document_get_filenum(m_document);++i) {
        ddjvu_fileinfo_t fi;
        if (ddjvu_document_get_fileinfo(m_document,i,&fi)!=DDJVU_JOB_OK)
            continue;

        QString id = fi.id;
        QString name = fi.name;
        QString title = fi.title;

        m_pageByName[id] = m_pageByName[name] = m_pageByName[title] = fi.pageno+1;
        m_titleByIndex[fi.pageno] = title;
    }
}

} // namespace deepin_reader
