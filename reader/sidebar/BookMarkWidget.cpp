// Copyright (C) 2019 ~ 2020 Uniontech Software Technology Co.,Ltd.
// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BookMarkWidget.h"
#include "DocSheet.h"
#include "SideBarImageListview.h"
#include "SideBarImageViewModel.h"
#include "BookMarkDelegate.h"
#include "SaveDialog.h"
#include "MsgHeader.h"
#include "ddlog.h"

#include <DHorizontalLine>
#include <DPushButton>
#include <DGuiApplicationHelper>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSet>

DGUI_USE_NAMESPACE

namespace {
constexpr int LEFT_MIN_HEIGHT = 80;
}

BookMarkWidget::BookMarkWidget(DocSheet *sheet, DWidget *parent)
    : BaseWidget(parent),
      m_sheet(sheet)
{
    qCDebug(appLog) << "Creating BookMarkWidget for document:"
                    << (sheet ? sheet->filePath() : "null");

    initWidget();
    onUpdateTheme();
}

BookMarkWidget::~BookMarkWidget() = default;

void BookMarkWidget::initWidget()
{
    qCDebug(appLog) << "Initializing BookMarkWidget";

    connect(DGuiApplicationHelper::instance(),
            &DGuiApplicationHelper::themeTypeChanged,
            this,
            &BookMarkWidget::onUpdateTheme);

    m_pImageListView = new SideBarImageListView(m_sheet, this);
    m_pImageListView->setAccessibleName("View_ImageList");
    m_pImageListView->setListType(E_SideBar::SIDE_BOOKMARK);
    m_pImageListView->setItemDelegate(new BookMarkDelegate(m_pImageListView));

    m_pAddBookMarkBtn = new DPushButton(this);
    m_pAddBookMarkBtn->setObjectName("BookmarkAddBtn");
    m_pAddBookMarkBtn->setAccessibleName("BookmarkAdd");
    m_pAddBookMarkBtn->setMinimumWidth(170);
    m_pAddBookMarkBtn->setText(tr("Add bookmark"));

    DFontSizeManager::instance()->bind(m_pAddBookMarkBtn, DFontSizeManager::T6);

    connect(m_pAddBookMarkBtn,
            &DPushButton::clicked,
            this,
            &BookMarkWidget::onAddBookMarkClicked);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(10, 6, 10, 6);
    buttonLayout->addWidget(m_pAddBookMarkBtn);

    auto *mainLayout = new QVBoxLayout;
    mainLayout->setContentsMargins(0, 10, 0, 0);
    mainLayout->setSpacing(0);

    mainLayout->addWidget(m_pImageListView);

    auto *line = new DHorizontalLine(this);
    line->setAccessibleName("BookMarkLine");
    mainLayout->addWidget(line);
    mainLayout->addLayout(buttonLayout);

    setLayout(mainLayout);

    connect(m_pImageListView,
            &SideBarImageListView::sigListMenuClick,
            this,
            &BookMarkWidget::onListMenuClick);

    m_pImageListView->setItemSize(QSize(LEFTMINWIDTH, LEFT_MIN_HEIGHT));

    qCDebug(appLog) << "BookMarkWidget initialization completed";
}

// ---------------------- Navigation helpers ----------------------

bool BookMarkWidget::isSheetValid() const
{
    if (m_sheet.isNull()) {
        qCWarning(appLog) << "Operation skipped: sheet is null";
        return false;
    }
    return true;
}

bool BookMarkWidget::isRowValid(int row) const
{
    if (!m_pImageListView)
        return false;

    auto *model = m_pImageListView->model();
    if (!model)
        return false;

    return row >= 0 && row < model->rowCount();
}


void BookMarkWidget::prevPage()
{
    if (!isSheetValid()) return;

    int row = m_pImageListView->currentIndex().row() - 1;
    if (!isRowValid(row)) return;

    m_sheet->jumpToIndex(
        m_pImageListView->getPageIndexForModelIndex(row));
}

void BookMarkWidget::nextPage()
{
    if (!isSheetValid()) return;

    int row = m_pImageListView->currentIndex().row() + 1;
    if (!isRowValid(row)) return;

    m_sheet->jumpToIndex(
        m_pImageListView->getPageIndexForModelIndex(row));
}

void BookMarkWidget::pageUp()
{
    if (!isSheetValid()) return;

    const QModelIndex idx = m_pImageListView->pageUpIndex();
    if (!idx.isValid()) return;

    m_sheet->jumpToIndex(
        m_pImageListView->getPageIndexForModelIndex(idx.row()));
}

void BookMarkWidget::pageDown()
{
    if (!isSheetValid()) return;

    const QModelIndex idx = m_pImageListView->pageDownIndex();
    if (!idx.isValid()) return;

    m_sheet->jumpToIndex(
        m_pImageListView->getPageIndexForModelIndex(idx.row()));
}

// ---------------------- State handling ----------------------

void BookMarkWidget::handleOpenSuccess()
{
    if (!isSheetValid()) return;

    if (bIshandOpenSuccess) return;
    bIshandOpenSuccess = true;

    const QSet<int> &pages = m_sheet->getBookMarkList();

    m_pAddBookMarkBtn->setEnabled(
        !pages.contains(m_sheet->currentIndex()));

    m_pImageListView->handleOpenSuccess();
}

void BookMarkWidget::handlePage(int index)
{
    bool hasBookmark = m_pImageListView->scrollToIndex(index);
    m_pAddBookMarkBtn->setDisabled(hasBookmark);
}

void BookMarkWidget::handleBookMark(int index, int state)
{
    if (!isSheetValid()) return;

    auto *model = m_pImageListView->getImageModel();
    if (!model) return;

    const int current = m_sheet->currentIndex();

    if (state) {
        if (current == index)
            m_pAddBookMarkBtn->setEnabled(false);
        model->insertPageIndex(index);
    } else {
        if (current == index)
            m_pAddBookMarkBtn->setEnabled(true);
        model->removePageIndex(index);
    }

    m_pImageListView->scrollToIndex(current, true);
}

// ---------------------- Delete operations ----------------------

void BookMarkWidget::deleteItemByKey()
{
    if (!isSheetValid()) return;

    int row = m_pImageListView->currentIndex().row();
    if (!isRowValid(row)) return;

    int pageIndex = m_pImageListView->getPageIndexForModelIndex(row);
    if (pageIndex >= 0)
        m_sheet->setBookMark(pageIndex, false);
}

void BookMarkWidget::deleteAllItem()
{
    if (!isSheetValid()) return;

    QList<int> bookmarks;
    const int rows = m_pImageListView->model()->rowCount();

    bookmarks.reserve(rows);

    for (int i = 0; i < rows; ++i) {
        int pageIndex = m_pImageListView->getPageIndexForModelIndex(i);
        if (pageIndex >= 0)
            bookmarks.append(pageIndex);
    }

    if (!bookmarks.isEmpty())
        m_sheet->setBookMarks(bookmarks, false);
}

// ---------------------- UI actions ----------------------

void BookMarkWidget::onAddBookMarkClicked()
{
    if (!isSheetValid()) return;

    m_sheet->setBookMark(m_sheet->currentIndex(), true);
}

void BookMarkWidget::adaptWindowSize(const double &scale)
{
    if (!isSheetValid()) return;

    m_pImageListView->setProperty("adaptScale", scale);
    m_pImageListView->setItemSize(
        QSize(static_cast<int>(LEFTMINWIDTH * scale), LEFT_MIN_HEIGHT));

    m_pImageListView->reset();
    m_pImageListView->scrollToIndex(m_sheet->currentIndex(), false);
}

void BookMarkWidget::showMenu()
{
    if (m_pImageListView && m_pImageListView->count() > 0)
        m_pImageListView->showMenu();
}

void BookMarkWidget::onUpdateTheme()
{
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, palette.color(QPalette::Base));

    setPalette(palette);
    m_pAddBookMarkBtn->setPalette(palette);
}

void BookMarkWidget::onListMenuClick(const int &type)
{
    if (type == E_BOOKMARK_DELETE) {
        deleteItemByKey();
        return;
    }

    if (type == E_BOOKMARK_DELETE_ALL) {
        int result = SaveDialog::showTipDialog(
            tr("Are you sure you want to delete all bookmarks?"), this);

        if (result == 1)
            deleteAllItem();
    }
}

// ---------------------- Accessibility ----------------------

void BookMarkWidget::setTabOrderWidget(QList<QWidget *> &tabWidgets)
{
    tabWidgets << m_pAddBookMarkBtn;
}
