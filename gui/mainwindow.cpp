/*
 * Copyright (C) 2015-2018 Olzhas Rakhimov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// @file

#include "mainwindow.h"
#include "ui_namedialog.h"
#include "ui_startpage.h"
//#include "ui_mainwindow.h"

#include <algorithm>
#include <sstream>
#include <type_traits>

#include <QApplication>
#include <QCoreApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QPrinter>
#include <QProgressDialog>
#include <QtConcurrent>
#include <QtOpenGL>

#include <kddockwidgets/DockWidget>

#include <boost/exception/get_error_info.hpp>
#include <boost/filesystem.hpp>

#include "src/env.h"
#include "src/error.h"
#include "src/expression/constant.h"
#include "src/expression/exponential.h"
#include "src/ext/algorithm.h"
#include "src/ext/find_iterator.h"
#include "src/ext/variant.h"
#include "src/initializer.h"
#include "src/project.h"
#include "src/reporter.h"
#include "src/serialization.h"
#include "src/xml.h"

#include "diagram.h"
#include "diagramview.h"
#include "elementcontainermodel.h"
#include "eventdialog.h"
#include "guiassert.h"
#include "importancetablemodel.h"
#include "modeltree.h"
#include "preferencesdialog.h"
#include "producttablemodel.h"
#include "reporttree.h"
#include "settingsdialog.h"
#include "translate.h"
#include "validator.h"

namespace scram::gui {

/// The dialog to set the model name.
class NameDialog : public QDialog, public Ui::NameDialog
{
public:
    /// @param[in,out] parent  The owner widget.
    explicit NameDialog(QWidget *parent) : QDialog(parent)
    {
        setupUi(this);
        nameLine->setValidator(Validator::name());
    }
};

/// The initial start tab.
class StartPage : public QWidget, public Ui::StartPage
{
public:
    /// @param[in,out] parent  The owner widget.
    explicit StartPage(QWidget *parent = nullptr) : QWidget(parent)
    {
        setupUi(this);
    }
};

/// The dialog to block user input while waiting for a long-running process.
class WaitDialog : public QProgressDialog
{
public:
    /// @param[in,out] parent  The owner widget.
    explicit WaitDialog(QWidget *parent) : QProgressDialog(parent)
    {
        setFixedSize(size());
        setWindowFlags(windowFlags() | Qt::MSWindowsFixedSizeDialogHint
                       | Qt::FramelessWindowHint);
        setCancelButton(nullptr);
        setRange(0, 0);
        setMinimumDuration(0);
    }

private:
    /// Intercepts disruptive keyboard presses.
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
            return event->accept();
        QProgressDialog::keyPressEvent(event);
    }
};

MainWindow::MainWindow(QWidget *parent)
	: KDDockWidgets::MainWindow(tr("Main Window"), KDDockWidgets::MainWindowOption_None, parent),
      m_undoStack(new QUndoStack(this)),
      m_zoomBox(new QComboBox), // Will be owned by the tool bar later.
      m_autoSaveTimer(new QTimer(this))
{
	setupUi();
    m_zoomBox->setEditable(true);
    m_zoomBox->setEnabled(false);
    m_zoomBox->setInsertPolicy(QComboBox::NoInsert);
    m_zoomBox->setValidator(Validator::percent());
	for (QAction *action : menuZoom->actions()) {
        m_zoomBox->addItem(action->text());
        connect(action, &QAction::triggered, m_zoomBox,
                [action, this] { m_zoomBox->setCurrentText(action->text()); });
    }
    m_zoomBox->setCurrentText(QStringLiteral("100%"));
	zoomToolBar->addWidget(m_zoomBox); // Transfer the ownership.

    setupStatusBar();
    setupActions();
    setupConnections();
    loadPreferences();
    setupStartPage();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
	QSizePolicy sizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	sizePolicy.setHorizontalStretch(0);
	sizePolicy.setVerticalStretch(0);
	sizePolicy.setHeightForWidth(this->sizePolicy().hasHeightForWidth());
	this->setSizePolicy(sizePolicy);
	this->setWindowTitle(QString::fromUtf8("SCRAM"));
	QIcon icon;
	icon.addFile(QString::fromUtf8(":/images/scram128x128.png"), QSize(), QIcon::Normal, QIcon::Off);
	this->setWindowIcon(icon);
	this->setAutoFillBackground(false);
	actionAboutQt = new QAction(this);
	actionAboutQt->setObjectName(QString::fromUtf8("actionAboutQt"));
	QIcon icon1;
	QString iconThemeName = QString::fromUtf8("help-about");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon1 = QIcon::fromTheme(iconThemeName);
	} else {
		icon1.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionAboutQt->setIcon(icon1);
	actionAboutScram = new QAction(this);
	actionAboutScram->setObjectName(QString::fromUtf8("actionAboutScram"));
	actionAboutScram->setIcon(icon1);
	actionExit = new QAction(this);
	actionExit->setObjectName(QString::fromUtf8("actionExit"));
	QIcon icon2;
	iconThemeName = QString::fromUtf8("application-exit");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon2 = QIcon::fromTheme(iconThemeName);
	} else {
		icon2.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionExit->setIcon(icon2);
	actionNewModel = new QAction(this);
	actionNewModel->setObjectName(QString::fromUtf8("actionNewModel"));
	QIcon icon3;
	iconThemeName = QString::fromUtf8("document-new");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon3 = QIcon::fromTheme(iconThemeName);
	} else {
		icon3.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionNewModel->setIcon(icon3);
	actionOpenFiles = new QAction(this);
	actionOpenFiles->setObjectName(QString::fromUtf8("actionOpenFiles"));
	QIcon icon4;
	iconThemeName = QString::fromUtf8("document-open");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon4 = QIcon::fromTheme(iconThemeName);
	} else {
		icon4.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionOpenFiles->setIcon(icon4);
	actionSave = new QAction(this);
	actionSave->setObjectName(QString::fromUtf8("actionSave"));
	actionSave->setEnabled(false);
	QIcon icon5;
	iconThemeName = QString::fromUtf8("document-save");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon5 = QIcon::fromTheme(iconThemeName);
	} else {
		icon5.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionSave->setIcon(icon5);
	actionSaveAs = new QAction(this);
	actionSaveAs->setObjectName(QString::fromUtf8("actionSaveAs"));
	actionSaveAs->setEnabled(false);
	QIcon icon6;
	iconThemeName = QString::fromUtf8("document-save-as");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon6 = QIcon::fromTheme(iconThemeName);
	} else {
		icon6.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionSaveAs->setIcon(icon6);
	actionPrint = new QAction(this);
	actionPrint->setObjectName(QString::fromUtf8("actionPrint"));
	actionPrint->setEnabled(false);
	QIcon icon7;
	iconThemeName = QString::fromUtf8("document-print");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon7 = QIcon::fromTheme(iconThemeName);
	} else {
		icon7.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionPrint->setIcon(icon7);
	actionExportAs = new QAction(this);
	actionExportAs->setObjectName(QString::fromUtf8("actionExportAs"));
	actionExportAs->setEnabled(false);
	QIcon icon8;
	iconThemeName = QString::fromUtf8("document-export");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon8 = QIcon::fromTheme(iconThemeName);
	} else {
		icon8.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionExportAs->setIcon(icon8);
	actionZoomIn = new QAction(this);
	actionZoomIn->setObjectName(QString::fromUtf8("actionZoomIn"));
	actionZoomIn->setEnabled(false);
	QIcon icon9;
	iconThemeName = QString::fromUtf8("zoom-in");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon9 = QIcon::fromTheme(iconThemeName);
	} else {
		icon9.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionZoomIn->setIcon(icon9);
	actionZoomOut = new QAction(this);
	actionZoomOut->setObjectName(QString::fromUtf8("actionZoomOut"));
	actionZoomOut->setEnabled(false);
	QIcon icon10;
	iconThemeName = QString::fromUtf8("zoom-out");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon10 = QIcon::fromTheme(iconThemeName);
	} else {
		icon10.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionZoomOut->setIcon(icon10);
	action400 = new QAction(this);
	action400->setObjectName(QString::fromUtf8("action400"));
	action400->setText(QString::fromUtf8("400%"));
	action400->setIconText(QString::fromUtf8("400%"));
#if QT_CONFIG(tooltip)
	action400->setToolTip(QString::fromUtf8("400%"));
#endif // QT_CONFIG(tooltip)
	action200 = new QAction(this);
	action200->setObjectName(QString::fromUtf8("action200"));
	action200->setText(QString::fromUtf8("200%"));
	action200->setIconText(QString::fromUtf8("200%"));
#if QT_CONFIG(tooltip)
	action200->setToolTip(QString::fromUtf8("200%"));
#endif // QT_CONFIG(tooltip)
	action150 = new QAction(this);
	action150->setObjectName(QString::fromUtf8("action150"));
	action150->setText(QString::fromUtf8("150%"));
	action150->setIconText(QString::fromUtf8("150%"));
#if QT_CONFIG(tooltip)
	action150->setToolTip(QString::fromUtf8("150%"));
#endif // QT_CONFIG(tooltip)
	action125 = new QAction(this);
	action125->setObjectName(QString::fromUtf8("action125"));
	action125->setText(QString::fromUtf8("125%"));
	action125->setIconText(QString::fromUtf8("125%"));
#if QT_CONFIG(tooltip)
	action125->setToolTip(QString::fromUtf8("125%"));
#endif // QT_CONFIG(tooltip)
	action100 = new QAction(this);
	action100->setObjectName(QString::fromUtf8("action100"));
	action100->setText(QString::fromUtf8("100%"));
	action100->setIconText(QString::fromUtf8("100%"));
#if QT_CONFIG(tooltip)
	action100->setToolTip(QString::fromUtf8("100%"));
#endif // QT_CONFIG(tooltip)
	action85 = new QAction(this);
	action85->setObjectName(QString::fromUtf8("action85"));
	action85->setText(QString::fromUtf8("85%"));
	action85->setIconText(QString::fromUtf8("85%"));
#if QT_CONFIG(tooltip)
	action85->setToolTip(QString::fromUtf8("85%"));
#endif // QT_CONFIG(tooltip)
	action50 = new QAction(this);
	action50->setObjectName(QString::fromUtf8("action50"));
	action50->setText(QString::fromUtf8("50%"));
	action50->setIconText(QString::fromUtf8("50%"));
#if QT_CONFIG(tooltip)
	action50->setToolTip(QString::fromUtf8("50%"));
#endif // QT_CONFIG(tooltip)
	action70 = new QAction(this);
	action70->setObjectName(QString::fromUtf8("action70"));
	action70->setText(QString::fromUtf8("70%"));
	action70->setIconText(QString::fromUtf8("70%"));
#if QT_CONFIG(tooltip)
	action70->setToolTip(QString::fromUtf8("70%"));
#endif // QT_CONFIG(tooltip)
	actionBestFit = new QAction(this);
	actionBestFit->setObjectName(QString::fromUtf8("actionBestFit"));
	actionBestFit->setEnabled(false);
	QIcon icon11;
	iconThemeName = QString::fromUtf8("zoom-fit-best");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon11 = QIcon::fromTheme(iconThemeName);
	} else {
		icon11.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionBestFit->setIcon(icon11);
	actionRun = new QAction(this);
	actionRun->setObjectName(QString::fromUtf8("actionRun"));
	actionRun->setEnabled(false);
	QIcon icon12;
	iconThemeName = QString::fromUtf8("utilities-terminal");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon12 = QIcon::fromTheme(iconThemeName);
	} else {
		icon12.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionRun->setIcon(icon12);
#if QT_CONFIG(shortcut)
	actionRun->setShortcut(QString::fromUtf8("Alt+R"));
#endif // QT_CONFIG(shortcut)
	actionSettings = new QAction(this);
	actionSettings->setObjectName(QString::fromUtf8("actionSettings"));
	QIcon icon13;
	iconThemeName = QString::fromUtf8("applications-system");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon13 = QIcon::fromTheme(iconThemeName);
	} else {
		icon13.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionSettings->setIcon(icon13);
#if QT_CONFIG(shortcut)
	actionSettings->setShortcut(QString::fromUtf8("Alt+S"));
#endif // QT_CONFIG(shortcut)
	actionModelToolBar = new QAction(this);
	actionModelToolBar->setObjectName(QString::fromUtf8("actionModelToolBar"));
	actionModelToolBar->setCheckable(true);
	actionZoomToolBar = new QAction(this);
	actionZoomToolBar->setObjectName(QString::fromUtf8("actionZoomToolBar"));
	actionZoomToolBar->setCheckable(true);
	actionAnalysisToolBar = new QAction(this);
	actionAnalysisToolBar->setObjectName(QString::fromUtf8("actionAnalysisToolBar"));
	actionAnalysisToolBar->setCheckable(true);
	actionData = new QAction(this);
	actionData->setObjectName(QString::fromUtf8("actionData"));
	actionData->setCheckable(true);
	actionReports = new QAction(this);
	actionReports->setObjectName(QString::fromUtf8("actionReports"));
	actionReports->setCheckable(true);
	actionPrintPreview = new QAction(this);
	actionPrintPreview->setObjectName(QString::fromUtf8("actionPrintPreview"));
	actionPrintPreview->setEnabled(false);
	QIcon icon14;
	iconThemeName = QString::fromUtf8("document-print-preview");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon14 = QIcon::fromTheme(iconThemeName);
	} else {
		icon14.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionPrintPreview->setIcon(icon14);
	actionAddElement = new QAction(this);
	actionAddElement->setObjectName(QString::fromUtf8("actionAddElement"));
	actionAddElement->setEnabled(false);
	QIcon icon15;
	iconThemeName = QString::fromUtf8("list-add");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon15 = QIcon::fromTheme(iconThemeName);
	} else {
		icon15.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionAddElement->setIcon(icon15);
	actionRemoveElement = new QAction(this);
	actionRemoveElement->setObjectName(QString::fromUtf8("actionRemoveElement"));
	actionRemoveElement->setEnabled(false);
	QIcon icon16;
	iconThemeName = QString::fromUtf8("list-remove");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon16 = QIcon::fromTheme(iconThemeName);
	} else {
		icon16.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionRemoveElement->setIcon(icon16);
	actionEditToolBar = new QAction(this);
	actionEditToolBar->setObjectName(QString::fromUtf8("actionEditToolBar"));
	actionEditToolBar->setCheckable(true);
	actionExportReportAs = new QAction(this);
	actionExportReportAs->setObjectName(QString::fromUtf8("actionExportReportAs"));
	actionExportReportAs->setEnabled(false);
	actionExportReportAs->setIcon(icon8);
	actionRenameModel = new QAction(this);
	actionRenameModel->setObjectName(QString::fromUtf8("actionRenameModel"));
	actionRenameModel->setEnabled(false);
	actionPreferences = new QAction(this);
	actionPreferences->setObjectName(QString::fromUtf8("actionPreferences"));
	QIcon icon17;
	iconThemeName = QString::fromUtf8("preferences-system");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon17 = QIcon::fromTheme(iconThemeName);
	} else {
		icon17.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	actionPreferences->setIcon(icon17);
	actionClearList = new QAction(this);
	actionClearList->setObjectName(QString::fromUtf8("actionClearList"));

	menuBar = new QMenuBar(this);
	menuBar->setObjectName(QString::fromUtf8("menuBar"));
	menuBar->setGeometry(QRect(0, 0, 640, 25));
	menuHelp = new QMenu(menuBar);
	menuHelp->setObjectName(QString::fromUtf8("menuHelp"));
	menuFile = new QMenu(menuBar);
	menuFile->setObjectName(QString::fromUtf8("menuFile"));
	menuRecentFiles = new QMenu(menuFile);
	menuRecentFiles->setObjectName(QString::fromUtf8("menuRecentFiles"));
	menuRecentFiles->setEnabled(false);
	QIcon icon18;
	iconThemeName = QString::fromUtf8("document-open-recent");
	if (QIcon::hasThemeIcon(iconThemeName)) {
		icon18 = QIcon::fromTheme(iconThemeName);
	} else {
		icon18.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
	}
	menuRecentFiles->setIcon(icon18);
	menuView = new QMenu(menuBar);
	menuView->setObjectName(QString::fromUtf8("menuView"));
	menuZoom = new QMenu(menuView);
	menuZoom->setObjectName(QString::fromUtf8("menuZoom"));
	menuZoom->setEnabled(false);
	menuToolbars = new QMenu(menuView);
	menuToolbars->setObjectName(QString::fromUtf8("menuToolbars"));
	menuAnalysis = new QMenu(menuBar);
	menuAnalysis->setObjectName(QString::fromUtf8("menuAnalysis"));
	menuEdit = new QMenu(menuBar);
	menuEdit->setObjectName(QString::fromUtf8("menuEdit"));
	this->setMenuBar(menuBar);
	statusBar = new QStatusBar(this);
	statusBar->setObjectName(QString::fromUtf8("statusBar"));
	this->setStatusBar(statusBar);
	modelToolBar = new QToolBar(this);
	modelToolBar->setObjectName(QString::fromUtf8("modelToolBar"));
	this->addToolBar(Qt::TopToolBarArea, modelToolBar);
	editToolBar = new QToolBar(this);
	editToolBar->setObjectName(QString::fromUtf8("editToolBar"));
	this->addToolBar(Qt::TopToolBarArea, editToolBar);
	zoomToolBar = new QToolBar(this);
	zoomToolBar->setObjectName(QString::fromUtf8("zoomToolBar"));
	zoomToolBar->setMovable(true);
	this->addToolBar(Qt::TopToolBarArea, zoomToolBar);
	analysisToolBar = new QToolBar(this);
	analysisToolBar->setObjectName(QString::fromUtf8("analysisToolBar"));
	this->addToolBar(Qt::TopToolBarArea, analysisToolBar);

	auto* modelDockWidget = new KDDockWidgets::DockWidget(QStringLiteral("Data"));
	modelTree = new QTreeView(modelDockWidget);
	modelTree->setObjectName(QString::fromUtf8("modelTree"));
	QSizePolicy sizePolicy1(QSizePolicy::Preferred, QSizePolicy::Expanding);
	sizePolicy1.setHorizontalStretch(0);
	sizePolicy1.setVerticalStretch(0);
	sizePolicy1.setHeightForWidth(modelTree->sizePolicy().hasHeightForWidth());
	modelTree->setSizePolicy(sizePolicy1);
	modelTree->setAnimated(true);
	modelTree->header()->setVisible(false);
	modelDockWidget->setWidget(modelTree);
	this->addDockWidget(modelDockWidget, KDDockWidgets::Location_OnLeft);

	auto* reportDockWidget = new KDDockWidgets::DockWidget(QStringLiteral("Reports"));
	reportTree = new QTreeView(reportDockWidget);
	reportTree->setObjectName(QString::fromUtf8("reportTree"));
	reportTree->setAnimated(true);
	reportTree->header()->setVisible(false);
	reportTree->header()->setDefaultSectionSize(0);
	reportDockWidget->setWidget(reportTree);
	this->addDockWidget(reportDockWidget, KDDockWidgets::Location_OnLeft);

	menuBar->addAction(menuFile->menuAction());
	menuBar->addAction(menuEdit->menuAction());
	menuBar->addAction(menuView->menuAction());
	menuBar->addAction(menuAnalysis->menuAction());
	menuBar->addAction(menuHelp->menuAction());
	menuHelp->addAction(actionAboutScram);
	menuHelp->addAction(actionAboutQt);
	menuFile->addAction(actionNewModel);
	menuFile->addAction(actionOpenFiles);
	menuFile->addAction(menuRecentFiles->menuAction());
	menuFile->addSeparator();
	menuFile->addAction(actionSave);
	menuFile->addAction(actionSaveAs);
	menuFile->addSeparator();
	menuFile->addAction(actionExportAs);
	menuFile->addAction(actionExportReportAs);
	menuFile->addSeparator();
	menuFile->addAction(actionPrintPreview);
	menuFile->addAction(actionPrint);
	menuFile->addSeparator();
	menuFile->addAction(actionExit);
	menuRecentFiles->addSeparator();
	menuRecentFiles->addAction(actionClearList);
	menuView->addAction(actionZoomIn);
	menuView->addAction(actionZoomOut);
	menuView->addAction(menuZoom->menuAction());
	menuView->addAction(actionBestFit);
	menuView->addSeparator();
	menuView->addAction(menuToolbars->menuAction());
	menuView->addSeparator();
	menuView->addAction(actionData);
	menuView->addAction(actionReports);
	menuZoom->addAction(action400);
	menuZoom->addAction(action200);
	menuZoom->addAction(action150);
	menuZoom->addAction(action125);
	menuZoom->addAction(action100);
	menuZoom->addAction(action85);
	menuZoom->addAction(action70);
	menuZoom->addAction(action50);
	menuToolbars->addAction(actionModelToolBar);
	menuToolbars->addAction(actionEditToolBar);
	menuToolbars->addAction(actionZoomToolBar);
	menuToolbars->addAction(actionAnalysisToolBar);
	menuAnalysis->addAction(actionSettings);
	menuAnalysis->addSeparator();
	menuAnalysis->addAction(actionRun);
	menuEdit->addSeparator();
	menuEdit->addAction(actionAddElement);
	menuEdit->addAction(actionRemoveElement);
	menuEdit->addSeparator();
	menuEdit->addAction(actionRenameModel);
	menuEdit->addSeparator();
	menuEdit->addAction(actionPreferences);
	modelToolBar->addAction(actionNewModel);
	modelToolBar->addAction(actionOpenFiles);
	modelToolBar->addAction(actionSave);
	modelToolBar->addAction(actionSaveAs);
	editToolBar->addSeparator();
	editToolBar->addAction(actionAddElement);
	editToolBar->addAction(actionRemoveElement);
	zoomToolBar->addAction(actionZoomIn);
	zoomToolBar->addAction(actionBestFit);
	zoomToolBar->addAction(actionZoomOut);
	analysisToolBar->addAction(actionSettings);
	analysisToolBar->addAction(actionRun);

	retranslateUi();
	QObject::connect(actionModelToolBar, SIGNAL(toggled(bool)), modelToolBar, SLOT(setVisible(bool)));
	QObject::connect(modelToolBar, SIGNAL(visibilityChanged(bool)), actionModelToolBar, SLOT(setChecked(bool)));
	QObject::connect(actionExit, SIGNAL(triggered()), this, SLOT(close()));
	QObject::connect(zoomToolBar, SIGNAL(visibilityChanged(bool)), actionZoomToolBar, SLOT(setChecked(bool)));
	QObject::connect(actionZoomToolBar, SIGNAL(toggled(bool)), zoomToolBar, SLOT(setVisible(bool)));
	QObject::connect(analysisToolBar, SIGNAL(visibilityChanged(bool)), actionAnalysisToolBar, SLOT(setChecked(bool)));
	QObject::connect(actionAnalysisToolBar, SIGNAL(toggled(bool)), analysisToolBar, SLOT(setVisible(bool)));
	QObject::connect(actionData, SIGNAL(toggled(bool)), modelDockWidget, SLOT(setVisible(bool)));
	QObject::connect(modelDockWidget, SIGNAL(visibilityChanged(bool)), actionData, SLOT(setChecked(bool)));
	QObject::connect(actionReports, SIGNAL(toggled(bool)), reportDockWidget, SLOT(setVisible(bool)));
	QObject::connect(reportDockWidget, SIGNAL(visibilityChanged(bool)), actionReports, SLOT(setChecked(bool)));
	QObject::connect(actionEditToolBar, SIGNAL(toggled(bool)), editToolBar, SLOT(setVisible(bool)));
	QObject::connect(editToolBar, SIGNAL(visibilityChanged(bool)), actionEditToolBar, SLOT(setChecked(bool)));

	QMetaObject::connectSlotsByName(this);
} // setupUi

void MainWindow::retranslateUi()
{
	actionAboutQt->setText(QCoreApplication::translate("MainWindow", "About &Qt", nullptr));
#if QT_CONFIG(statustip)
	actionAboutQt->setStatusTip(QCoreApplication::translate("MainWindow", "About the Qt toolkit", nullptr));
#endif // QT_CONFIG(statustip)
	actionAboutScram->setText(QCoreApplication::translate("MainWindow", "About &SCRAM", nullptr));
	actionExit->setText(QCoreApplication::translate("MainWindow", "E&xit", nullptr));
#if QT_CONFIG(tooltip)
	actionExit->setToolTip(QCoreApplication::translate("MainWindow", "Exit the Application", nullptr));
#endif // QT_CONFIG(tooltip)
	actionNewModel->setText(QCoreApplication::translate("MainWindow", "&New Model", nullptr));
#if QT_CONFIG(tooltip)
	actionNewModel->setToolTip(QCoreApplication::translate("MainWindow", "Create a New Model", nullptr));
#endif // QT_CONFIG(tooltip)
	actionOpenFiles->setText(QCoreApplication::translate("MainWindow", "&Open Model Files...", nullptr));
#if QT_CONFIG(tooltip)
	actionOpenFiles->setToolTip(QCoreApplication::translate("MainWindow", "Open Model Files", nullptr));
#endif // QT_CONFIG(tooltip)
	actionSave->setText(QCoreApplication::translate("MainWindow", "&Save Model", nullptr));
	actionSaveAs->setText(QCoreApplication::translate("MainWindow", "Save Model &As...", nullptr));
	actionPrint->setText(QCoreApplication::translate("MainWindow", "&Print...", nullptr));
#if QT_CONFIG(tooltip)
	actionPrint->setToolTip(QCoreApplication::translate("MainWindow", "Print", nullptr));
#endif // QT_CONFIG(tooltip)
	actionExportAs->setText(QCoreApplication::translate("MainWindow", "&Export As...", nullptr));
	actionZoomIn->setText(QCoreApplication::translate("MainWindow", "Zoom &In", nullptr));
	actionZoomOut->setText(QCoreApplication::translate("MainWindow", "Zoom &Out", nullptr));
	actionBestFit->setText(QCoreApplication::translate("MainWindow", "Best &Fit", nullptr));
	actionRun->setText(QCoreApplication::translate("MainWindow", "&Run", "execute analysis"));
	actionRun->setIconText(QCoreApplication::translate("MainWindow", "Run Analysis", nullptr));
#if QT_CONFIG(tooltip)
	actionRun->setToolTip(QCoreApplication::translate("MainWindow", "Run Analysis", nullptr));
#endif // QT_CONFIG(tooltip)
	actionSettings->setText(QCoreApplication::translate("MainWindow", "&Settings...", "analysis configuration"));
	actionSettings->setIconText(QCoreApplication::translate("MainWindow", "Analysis Settings", nullptr));
#if QT_CONFIG(tooltip)
	actionSettings->setToolTip(QCoreApplication::translate("MainWindow", "Analysis Settings", nullptr));
#endif // QT_CONFIG(tooltip)
	actionModelToolBar->setText(QCoreApplication::translate("MainWindow", "&Model", nullptr));
	actionZoomToolBar->setText(QCoreApplication::translate("MainWindow", "&Zoom", nullptr));
	actionAnalysisToolBar->setText(QCoreApplication::translate("MainWindow", "&Analysis", nullptr));
	actionData->setText(QCoreApplication::translate("MainWindow", "&Data", nullptr));
	actionReports->setText(QCoreApplication::translate("MainWindow", "&Reports", nullptr));
	actionPrintPreview->setText(QCoreApplication::translate("MainWindow", "Print Previe&w...", nullptr));
	actionAddElement->setText(QCoreApplication::translate("MainWindow", "&Add Element", nullptr));
	actionRemoveElement->setText(QCoreApplication::translate("MainWindow", "Re&move Element", nullptr));
	actionEditToolBar->setText(QCoreApplication::translate("MainWindow", "&Edit", nullptr));
	actionExportReportAs->setText(QCoreApplication::translate("MainWindow", "Export &Report As...", nullptr));
	actionRenameModel->setText(QCoreApplication::translate("MainWindow", "Re&name Model", nullptr));
	actionPreferences->setText(QCoreApplication::translate("MainWindow", "&Preferences...", nullptr));
	actionClearList->setText(QCoreApplication::translate("MainWindow", "&Clear List", nullptr));
	menuHelp->setTitle(QCoreApplication::translate("MainWindow", "&Help", nullptr));
	menuFile->setTitle(QCoreApplication::translate("MainWindow", "&File", nullptr));
	menuRecentFiles->setTitle(QCoreApplication::translate("MainWindow", "Recent &Files", nullptr));
	menuView->setTitle(QCoreApplication::translate("MainWindow", "&View", nullptr));
	menuZoom->setTitle(QCoreApplication::translate("MainWindow", "&Zoom", nullptr));
	menuToolbars->setTitle(QCoreApplication::translate("MainWindow", "&Toolbars", nullptr));
	menuAnalysis->setTitle(QCoreApplication::translate("MainWindow", "&Analysis", nullptr));
	menuEdit->setTitle(QCoreApplication::translate("MainWindow", "&Edit", nullptr));
	modelToolBar->setWindowTitle(QCoreApplication::translate("MainWindow", "Model Tool Bar", nullptr));
	editToolBar->setWindowTitle(QCoreApplication::translate("MainWindow", "Edit Tool Bar", nullptr));
	zoomToolBar->setWindowTitle(QCoreApplication::translate("MainWindow", "Zoom Tool Bar", nullptr));
	analysisToolBar->setWindowTitle(QCoreApplication::translate("MainWindow", "Analysis Tool Bar", nullptr));
//	modelDockWidget->setWindowTitle(QCoreApplication::translate("MainWindow", "Data", nullptr));
//	reportDockWidget->setWindowTitle(QCoreApplication::translate("MainWindow", "Reports", nullptr));
//	(void)MainWindow;
} // retranslateUi

namespace { // Error message dialog for SCRAM exceptions.

void displayError(const scram::IOError &err, const QString &text,
                  QWidget *parent = nullptr)
{
    QMessageBox message(QMessageBox::Critical, _("IO Error"), text,
                        QMessageBox::Ok, parent);

    const std::string *filename =
        boost::get_error_info<boost::errinfo_file_name>(err);
    GUI_ASSERT(filename, );
    message.setInformativeText(
        _("File: %1").arg(QString::fromStdString(*filename)));

    std::stringstream detail;
    if (const std::string *mode =
            boost::get_error_info<boost::errinfo_file_open_mode>(err)) {
        detail << "Open mode: " << *mode << "\n";
    }
    if (const int *errnum = boost::get_error_info<boost::errinfo_errno>(err)) {
        detail << "Error code: " << *errnum << "\n";
        detail << "Error string: " << std::strerror(*errnum) << "\n";
    }
    detail << "\n" << err.what() << std::endl;
    message.setDetailedText(QString::fromStdString(detail.str()));

    message.exec();
}

template <class Tag>
void displayErrorInfo(QString tag_string, const scram::Error &err,
                      QString *info)
{
    if (const auto *value = boost::get_error_info<Tag>(err)) {
        std::stringstream ss;
        ss << *value;
        auto value_string = QString::fromStdString(ss.str());

        //: Error information tag and its value.
        info->append(_("%1: %2").arg(tag_string, value_string));
        info->append(QStringLiteral("\n"));
    }
}

void displayError(const scram::Error &err, const QString &title,
                  const QString &text, QWidget *parent = nullptr)
{
    QMessageBox message(QMessageBox::Critical, title, text, QMessageBox::Ok,
                        parent);
    QString info;
    auto newLine = [&info] { info.append(QStringLiteral("\n")); };

    displayErrorInfo<errinfo_value>(_("Value"), err, &info);

    if (const std::string *filename =
            boost::get_error_info<boost::errinfo_file_name>(err)) {
        info.append(_("File: %1").arg(QString::fromStdString(*filename)));
        newLine();
        if (const int *line =
                boost::get_error_info<boost::errinfo_at_line>(err)) {
            info.append(_("Line: %1").arg(*line));
            newLine();
        }
    }

    displayErrorInfo<mef::errinfo_connective>(_("MEF Connective"), err, &info);
    displayErrorInfo<mef::errinfo_reference>(_("MEF reference"), err, &info);
    displayErrorInfo<mef::errinfo_base_path>(_("MEF base path"), err, &info);
    displayErrorInfo<mef::errinfo_element_id>(_("MEF Element ID"), err, &info);
    displayErrorInfo<mef::errinfo_element_type>(_("MEF Element type"), err,
                                                &info);
    displayErrorInfo<mef::errinfo_container_id>(_("MEF Container"), err, &info);
    displayErrorInfo<mef::errinfo_container_type>(_("MEF Container type"), err,
                                                  &info);
    displayErrorInfo<mef::errinfo_attribute>(_("MEF Attribute"), err, &info);
    displayErrorInfo<mef::errinfo_cycle>(_("Cycle"), err, &info);

    if (const std::string *xml_element =
            boost::get_error_info<xml::errinfo_element>(err)) {
        info.append(
            _("XML element: %1").arg(QString::fromStdString(*xml_element)));
        newLine();
    }
    if (const std::string *xml_attribute =
            boost::get_error_info<xml::errinfo_attribute>(err)) {
        info.append(
            _("XML attribute: %1").arg(QString::fromStdString(*xml_attribute)));
        newLine();
    }
    message.setInformativeText(info);

    std::stringstream detail;
    detail << boost::core::demangled_name(typeid(err)) << "\n\n";
    detail << err.what() << std::endl;
    message.setDetailedText(QString::fromStdString(detail.str()));

    message.exec();
}

} // namespace

bool MainWindow::setProjectFile(const std::string &projectFilePath,
                                std::vector<std::string> inputFiles)
{
    try {
        Project project(projectFilePath);
        inputFiles.insert(inputFiles.begin(), project.input_files().begin(),
                          project.input_files().end());
        mef::Initializer(inputFiles, project.settings());
        if (!addInputFiles(inputFiles))
            return false;
        m_settings = project.settings();
    } catch (const scram::IOError &err) {
        displayError(err, _("Configuration file error"), this);
        return false;
    } catch (const scram::xml::Error &err) {
        displayError(err, _("XML Validity Error"),
                     _("Invalid configuration file"), this);
        return false;
    } catch (const scram::SettingsError &err) {
        displayError(err, _("Configuration Error"), _("Invalid configurations"),
                     this);
        return false;
    } catch (const scram::VersionError &err) {
        displayError(err, _("Version Error"), _("Version incompatibility"),
                     this);
        return false;
    }
    return true;
}

bool MainWindow::addInputFiles(const std::vector<std::string> &inputFiles)
{
    static xml::Validator validator(env::install_dir()
                                    + "/share/scram/gui.rng");

    if (inputFiles.empty())
        return true;
    if (isWindowModified() && !saveModel())
        return false;

    try {
        std::vector<std::string> allInput = m_inputFiles;
        allInput.insert(allInput.end(), inputFiles.begin(), inputFiles.end());
        std::unique_ptr<mef::Model> newModel = [this, &allInput] {
            return mef::Initializer(allInput, m_settings,
                                    /*allow_extern=*/false, &validator)
                .model();
        }();

        for (const mef::FaultTree &faultTree : newModel->fault_trees()) {
            if (faultTree.top_events().size() != 1) {
                QMessageBox::critical(
                    this, _("Initialization Error"),
                    //: Single top/root event fault tree are expected by GUI.
                    _("Fault tree '%1' must have a single top-gate.")
                        .arg(QString::fromStdString(faultTree.name())));
                return false;
            }
        }

        m_model = std::move(newModel);
        m_inputFiles = std::move(allInput);
    } catch (const scram::IOError &err) {
        displayError(err, _("Input file error"), this);
        return false;
    } catch (const scram::xml::Error &err) {
        displayError(err, _("XML Validity Error"), _("Invalid input file"),
                     this);
        return false;
    } catch (const scram::mef::ValidityError &err) {
        displayError(err,
                     //: The error upon initialization from a file.
                     _("Initialization Error"), _("Invalid input model"), this);
        return false;
    }

    emit projectChanged();
    return true;
}

void MainWindow::setupStatusBar()
{
    m_searchBar = new QLineEdit;
    m_searchBar->setHidden(true);
    m_searchBar->setFrame(false);
    m_searchBar->setMaximumHeight(m_searchBar->fontMetrics().height());
    m_searchBar->setSizePolicy(QSizePolicy::MinimumExpanding,
                               QSizePolicy::Fixed);
    //: The search bar.
    m_searchBar->setPlaceholderText(_("Find/Filter (Perl Regex)"));
	statusBar->addPermanentWidget(m_searchBar);
}

void MainWindow::setupActions()
{
	connect(actionAboutQt, &QAction::triggered, qApp,
            &QApplication::aboutQt);
	connect(actionAboutScram, &QAction::triggered, this, [this] {
        QString legal = QStringLiteral(
            "This program is distributed in the hope that it will be useful, "
            "but WITHOUT ANY WARRANTY; without even the implied warranty of "
            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the "
            "GNU General Public License for more details.");
        QMessageBox::about(
            this, _("About SCRAM"),
            _("<h1>SCRAM %1</h1>"
              "The GUI front-end for SCRAM,<br/>"
              "a command-line risk analysis multi-tool.<br/><br/>"
              "License: GPLv3+<br/>"
              "Homepage: <a href=\"%2\">%2</a><br/>"
              "Technical Support: <a href=\"%3\">%3</a><br/>"
              "Bug Tracker: <a href=\"%4\">%4</a><br/><br/>%5")
                .arg(QCoreApplication::applicationVersion(),
                     QStringLiteral("https://scram-pra.org"),
                     QStringLiteral("scram-users@googlegroups.com"),
                     QStringLiteral("https://github.com/rakhimov/scram/issues"),
                     legal));
    });

    // File menu actions.
	actionExit->setShortcut(QKeySequence::Quit);

	actionNewModel->setShortcut(QKeySequence::New);
	connect(actionNewModel, &QAction::triggered, this,
            &MainWindow::createNewModel);

	actionOpenFiles->setShortcut(QKeySequence::Open);
	connect(actionOpenFiles, &QAction::triggered, this,
            [this] { openFiles(); });

	actionSave->setShortcut(QKeySequence::Save);
	connect(actionSave, &QAction::triggered, this, &MainWindow::saveModel);

	actionSaveAs->setShortcut(QKeySequence::SaveAs);
	connect(actionSaveAs, &QAction::triggered, this,
            &MainWindow::saveModelAs);

	actionPrint->setShortcut(QKeySequence::Print);

	connect(actionExportReportAs, &QAction::triggered, this,
            &MainWindow::exportReportAs);

	QAction *menuRecentFilesStart = menuRecentFiles->actions().front();
    for (QAction *&fileAction : m_recentFileActions) {
        fileAction = new QAction(this);
        fileAction->setVisible(false);
		menuRecentFiles->insertAction(menuRecentFilesStart, fileAction);
        connect(fileAction, &QAction::triggered, this, [this, fileAction] {
            auto filePath = fileAction->text();
            GUI_ASSERT(!filePath.isEmpty(), );
            if (addInputFiles({filePath.toStdString()}))
                updateRecentFiles({filePath});
        });
    }
	connect(actionClearList, &QAction::triggered, this,
            [this] { updateRecentFiles({}); });

    // View menu actions.
	actionZoomIn->setShortcut(QKeySequence::ZoomIn);
	actionZoomOut->setShortcut(QKeySequence::ZoomOut);

    // Edit menu actions.
	actionRemoveElement->setShortcut(QKeySequence::Delete);
	connect(actionAddElement, &QAction::triggered, this,
            &MainWindow::addElement);
	connect(actionRenameModel, &QAction::triggered, this, [this] {
        NameDialog nameDialog(this);
        if (!m_model->HasDefaultName())
            nameDialog.nameLine->setText(m_guiModel->id());
        if (nameDialog.exec() == QDialog::Accepted) {
            QString name = nameDialog.nameLine->text();
            if (name != QString::fromStdString(m_model->GetOptionalName())) {
                m_undoStack->push(new model::Model::SetName(std::move(name),
                                                            m_guiModel.get()));
            }
        }
    });
	connect(actionPreferences, &QAction::triggered, this, [this] {
        PreferencesDialog dialog(&m_preferences, m_undoStack, m_autoSaveTimer,
                                 this);
        dialog.exec();
    });

    // Undo/Redo actions
    m_undoAction = m_undoStack->createUndoAction(this, _("Undo:"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo")));

    m_redoAction = m_undoStack->createRedoAction(this, _("Redo:"));
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-redo")));

	menuEdit->insertAction(menuEdit->actions().front(), m_redoAction);
	menuEdit->insertAction(m_redoAction, m_undoAction);
	editToolBar->insertAction(editToolBar->actions().front(),
                                  m_redoAction);
	editToolBar->insertAction(m_redoAction, m_undoAction);
	connect(m_undoStack, &QUndoStack::cleanChanged, actionSave,
            &QAction::setDisabled);
    connect(m_undoStack, &QUndoStack::cleanChanged,
            [this](bool clean) { setWindowModified(!clean); });

    // Search/filter bar shortcuts.
    auto *searchAction = new QAction(this);
    searchAction->setShortcuts({QKeySequence::Find, Qt::Key_Slash});
    m_searchBar->addAction(searchAction);
    connect(searchAction, &QAction::triggered, [this] {
        if (m_searchBar->isHidden())
            return;
        m_searchBar->setFocus();
        m_searchBar->selectAll();
    });

    // Providing shortcuts for the tab widget manipulations.
    auto *closeCurrentTab = new QAction(this);
    auto *nextTab = new QAction(this);
    auto *prevTab = new QAction(this);

    closeCurrentTab->setShortcut(QKeySequence::Close);
    nextTab->setShortcut(QKeySequence::NextChild);
    // QTBUG-15746: QKeySequence::PreviousChild does not work.
    prevTab->setShortcut(Qt::CTRL | Qt::Key_Backtab);

//	tabWidget->addAction(closeCurrentTab);
//	tabWidget->addAction(nextTab);
//	tabWidget->addAction(prevTab);

//    auto switchTab = [this](bool toNext) {
//		int numTabs = tabWidget->count();
//        if (!numTabs)
//            return;
//		int currentIndex = tabWidget->currentIndex();
//        int nextIndex = [currentIndex, numTabs, toNext] {
//            int ret = currentIndex + (toNext ? 1 : -1);
//            if (ret < 0)
//                return numTabs - 1;
//            if (ret >= numTabs)
//                return 0;
//            return ret;
//        }();
//		tabWidget->setCurrentIndex(nextIndex);
//    };

//	connect(closeCurrentTab, &QAction::triggered, tabWidget,
//			[this] { MainWindow::closeTab(tabWidget->currentIndex()); });
//	connect(nextTab, &QAction::triggered, tabWidget,
//            [switchTab] { switchTab(true); });
//	connect(prevTab, &QAction::triggered, tabWidget,
//            [switchTab] { switchTab(false); });
}

void MainWindow::setupConnections()
{
	connect(modelTree, &QTreeView::activated, this,
            &MainWindow::activateModelTree);
	connect(reportTree, &QTreeView::activated, this,
            &MainWindow::activateReportTree);
//	connect(tabWidget, &QTabWidget::tabCloseRequested, this,
//            &MainWindow::closeTab);

	connect(actionSettings, &QAction::triggered, this, [this] {
        SettingsDialog dialog(m_settings, this);
        if (dialog.exec() == QDialog::Accepted)
            m_settings = dialog.settings();
    });
	connect(actionRun, &QAction::triggered, this, &MainWindow::runAnalysis);

    connect(this, &MainWindow::projectChanged, [this] {
        m_undoStack->clear();
        setWindowTitle(QStringLiteral("%1[*]").arg(getModelNameForTitle()));
		actionSaveAs->setEnabled(true);
		actionAddElement->setEnabled(true);
		actionRenameModel->setEnabled(true);
		actionRun->setEnabled(true);
        resetModelTree();
        resetReportTree(nullptr);
    });
	connect(m_undoStack, &QUndoStack::indexChanged, reportTree, [this] {
        if (m_analysis)
            resetReportTree(nullptr);
    });
    connect(m_autoSaveTimer, &QTimer::timeout, this,
            &MainWindow::autoSaveModel);
}

void MainWindow::loadPreferences()
{
    m_preferences.beginGroup(QStringLiteral("MainWindow"));
    restoreGeometry(
        m_preferences.value(QStringLiteral("geometry")).toByteArray());
    restoreState(m_preferences.value(QStringLiteral("state")).toByteArray(),
                 LAYOUT_VERSION);
    m_preferences.endGroup();

    m_undoStack->setUndoLimit(
        m_preferences.value(QStringLiteral("undoLimit"), 0).toInt());

    GUI_ASSERT(m_autoSaveTimer->isActive() == false, );
    int interval =
        m_preferences.value(QStringLiteral("autoSave"), 300000).toInt();
    if (interval)
        m_autoSaveTimer->start(interval);

    updateRecentFiles(
        m_preferences.value(QStringLiteral("recentFiles")).toStringList());
}

void MainWindow::savePreferences()
{
    m_preferences.beginGroup(QStringLiteral("MainWindow"));
    m_preferences.setValue(QStringLiteral("geometry"), saveGeometry());
    m_preferences.setValue(QStringLiteral("state"), saveState(LAYOUT_VERSION));
    m_preferences.endGroup();

    QStringList fileList;
    for (QAction *fileAction : m_recentFileActions) {
        if (!fileAction->isVisible())
            break;
        fileList.push_back(fileAction->text());
    }
    m_preferences.setValue(QStringLiteral("recentFiles"), fileList);
}

void MainWindow::setupStartPage()
{
	auto* dock = new KDDockWidgets::DockWidget(QStringLiteral("StartPage"));

	auto *startPage = new StartPage(dock);
    QString examplesDir =
        QString::fromStdString(env::install_dir() + "/share/scram/input");
    startPage->exampleModelsButton->setEnabled(QDir(examplesDir).exists());
    connect(startPage->newModelButton, &QAbstractButton::clicked,
			actionNewModel, &QAction::trigger);
    connect(startPage->openModelButton, &QAbstractButton::clicked,
			actionOpenFiles, &QAction::trigger);
    connect(startPage->exampleModelsButton, &QAbstractButton::clicked, this,
            [this, examplesDir] { openFiles(examplesDir); });

	dock->setTitle(startPage->windowTitle());
	dock->setIcon(startPage->windowIcon());
	dock->setWidget(startPage);
	addDockWidget(dock, KDDockWidgets::Location_OnRight);

    startPage->recentFilesBox->setVisible(
        m_recentFileActions.front()->isVisible());
    for (QAction *fileAction : m_recentFileActions) {
        if (!fileAction->isVisible())
            break;
        auto *button =
            new QCommandLinkButton(QFileInfo(fileAction->text()).fileName());
        button->setToolTip(fileAction->text());
        startPage->recentFilesBox->layout()->addWidget(button);
        connect(button, &QAbstractButton::clicked, fileAction,
                &QAction::trigger);
    }
}

QString MainWindow::getModelNameForTitle()
{
    return m_model->HasDefaultName() ? _("Unnamed Model")
                                     : QString::fromStdString(m_model->name());
}

void MainWindow::createNewModel()
{
    if (isWindowModified()) {
        QMessageBox::StandardButton answer = QMessageBox::question(
            this, _("Save Model?"),
            _("Save changes to model '%1' before closing?")
                .arg(getModelNameForTitle()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Save && !saveModel())
            return;
    }

    m_inputFiles.clear();
    m_model = std::make_unique<mef::Model>();

    emit projectChanged();
}

void MainWindow::openFiles(QString directory)
{
    QStringList filenames = QFileDialog::getOpenFileNames(
        this, _("Open Model Files"), directory,
        QStringLiteral("%1 (*.mef *.opsa *.opsa-mef *.xml);;%2 (*.*)")
            .arg(_("Model Exchange Format"), _("All files")));
    if (filenames.empty())
        return;
    std::vector<std::string> inputFiles;
    for (const auto &filename : filenames)
        inputFiles.push_back(filename.toStdString());
    if (addInputFiles(inputFiles))
        updateRecentFiles(filenames);
}

void MainWindow::autoSaveModel()
{
    if (!isWindowModified() || m_inputFiles.empty() || m_inputFiles.size() > 1)
        return;
    saveToFile(m_inputFiles.front());
}

bool MainWindow::saveModel()
{
    if (m_inputFiles.empty() || m_inputFiles.size() > 1)
        return saveModelAs();
    return saveToFile(m_inputFiles.front());
}

bool MainWindow::saveModelAs()
{
    QString filename = QFileDialog::getSaveFileName(
        this, _("Save Model As"), QDir::homePath(),
        QStringLiteral("%1 (*.mef *.opsa *.opsa-mef *.xml);;%2 (*.*)")
            .arg(_("Model Exchange Format"), _("All files")));
    if (filename.isNull())
        return false;
    return saveToFile(filename.toStdString());
}

bool MainWindow::saveToFile(std::string destination)
{
    GUI_ASSERT(!destination.empty(), false);
    GUI_ASSERT(m_model, false);

    namespace fs = boost::filesystem;
    fs::path temp_file = destination + "." + fs::unique_path().string();

    try {
        mef::Serialize(*m_model, temp_file.string());
        try {
            fs::rename(temp_file, destination);
        } catch (const fs::filesystem_error &err) {
            SCRAM_THROW(IOError(err.what()))
                << boost::errinfo_file_name(destination)
                << boost::errinfo_errno(err.code().value());
        }
    } catch (const IOError &err) {
        displayError(err, _("Save error", "error on saving to file"), this);
        return false;
    }
    m_undoStack->setClean();
    m_inputFiles.clear();
    m_inputFiles.push_back(std::move(destination));
    return true;
}

void MainWindow::updateRecentFiles(QStringList filePaths)
{
	menuRecentFiles->setEnabled(!filePaths.empty());
    if (filePaths.empty()) {
        for (QAction *fileAction : m_recentFileActions)
            fileAction->setVisible(false);
        return;
    }

    int remainingCapacity = m_recentFileActions.size() - filePaths.size();
    for (QAction *fileAction : m_recentFileActions) {
        if (remainingCapacity <= 0)
            break;
        if (!fileAction->isVisible())
            break;
        if (filePaths.contains(fileAction->text()))
            continue;
        filePaths.push_back(fileAction->text());
        --remainingCapacity;
    }
    auto it = m_recentFileActions.begin();
    const auto &constFilePaths = filePaths; // Detach prevention w/ for-each.
    for (const QString &filePath : constFilePaths) {
        if (it == m_recentFileActions.end())
            break;
        (*it)->setText(filePath);
        (*it)->setVisible(true);
        ++it;
    }
    for (; it != m_recentFileActions.end(); ++it)
        (*it)->setVisible(false);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    savePreferences();

    if (!isWindowModified())
        return event->accept();

    QMessageBox::StandardButton answer = QMessageBox::question(
        this, _("Save Model?"),
        _("Save changes to model '%1' before closing?")
            .arg(getModelNameForTitle()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel)
        return event->ignore();
    if (answer == QMessageBox::Discard)
        return event->accept();

    return saveModel() ? event->accept() : event->ignore();
}

void MainWindow::closeTab(int index)
{
//    if (index < 0)
//        return;
//    // Ensure show/hide order.
//	if (index == tabWidget->currentIndex()) {
//		int num_tabs = tabWidget->count();
//        if (num_tabs > 1) {
//			tabWidget->setCurrentIndex(index == (num_tabs - 1) ? index - 1
//                                                                   : index + 1);
//        }
//    }
//	auto *widget = tabWidget->widget(index);
//	tabWidget->removeTab(index);
//    delete widget;
}

void MainWindow::runAnalysis()
{
    GUI_ASSERT(m_model, );
    if (m_settings.probability_analysis()
        && ext::any_of(m_model->basic_events(),
                       [](const mef::BasicEvent &basicEvent) {
                           return !basicEvent.HasExpression();
                       })) {
        QMessageBox::critical(this, _("Validation Error"),
                              _("Not all basic events have expressions "
                                "for probability analysis."));
        return;
    }
    WaitDialog progress(this);
    //: This is a message shown during the analysis run.
    progress.setLabelText(_("Running analysis..."));
    auto analysis =
        std::make_unique<core::RiskAnalysis>(m_model.get(), m_settings);
    QFutureWatcher<void> futureWatcher;
    connect(&futureWatcher, SIGNAL(finished()), &progress, SLOT(reset()));
    futureWatcher.setFuture(
        QtConcurrent::run([&analysis] { analysis->Analyze(); }));
    progress.exec();
    futureWatcher.waitForFinished();
    resetReportTree(std::move(analysis));
}

void MainWindow::exportReportAs()
{
    GUI_ASSERT(m_analysis, );
    QString filename = QFileDialog::getSaveFileName(
        this, _("Export Report As"), QDir::homePath(),
        QStringLiteral("%1 (*.mef *.opsa *.opsa-mef *.xml);;%2 (*.*)")
            .arg(_("Model Exchange Format"), _("All files")));
    if (filename.isNull())
        return;
    try {
        Reporter().Report(*m_analysis, filename.toStdString());
    } catch (const IOError &err) {
        displayError(err, _("Reporting error"), this);
    }
}

void MainWindow::setupZoomableView(ZoomableView *view)
{
    struct ZoomFilter : public QObject
    {
        ZoomFilter(ZoomableView *zoomable, MainWindow *window)
            : QObject(zoomable), m_window(window), m_zoomable(zoomable)
        {
        }
        bool eventFilter(QObject *object, QEvent *event) override
        {
            auto setEnabled = [this](bool state) {
                m_window->m_zoomBox->setEnabled(state);
				m_window->actionZoomIn->setEnabled(state);
				m_window->actionZoomIn->setEnabled(state);
				m_window->actionZoomOut->setEnabled(state);
				m_window->actionBestFit->setEnabled(state);
				m_window->menuZoom->setEnabled(state);
            };

            if (event->type() == QEvent::Show) {
                setEnabled(true);
                m_window->m_zoomBox->setCurrentText(
                    QStringLiteral("%1%").arg(m_zoomable->getZoom()));

                connect(m_zoomable, &ZoomableView::zoomChanged,
                        m_window->m_zoomBox, [this](int level) {
                            m_window->m_zoomBox->setCurrentText(
                                QStringLiteral("%1%").arg(level));
                        });
                connect(m_window->m_zoomBox, &QComboBox::currentTextChanged,
                        m_zoomable, [this](QString text) {
                            // Check if the user editing the box.
                            if (m_window->m_zoomBox->lineEdit()->isModified())
                                return;
                            text.remove(QLatin1Char('%'));
                            m_zoomable->setZoom(text.toInt());
                        });
                connect(m_window->m_zoomBox->lineEdit(),
                        &QLineEdit::editingFinished, m_zoomable, [this] {
                            QString text = m_window->m_zoomBox->currentText();
                            text.remove(QLatin1Char('%'));
                            m_zoomable->setZoom(text.toInt());
                        });
				connect(m_window->actionZoomIn, &QAction::triggered,
                        m_zoomable, [this] { m_zoomable->zoomIn(5); });
				connect(m_window->actionZoomOut, &QAction::triggered,
                        m_zoomable, [this] { m_zoomable->zoomOut(5); });
				connect(m_window->actionBestFit, &QAction::triggered,
                        m_zoomable, &ZoomableView::zoomBestFit);
            } else if (event->type() == QEvent::Hide) {
                setEnabled(false);
                disconnect(m_window->m_zoomBox->lineEdit(), 0, m_zoomable, 0);
                disconnect(m_zoomable, 0, m_window->m_zoomBox, 0);
                disconnect(m_window->m_zoomBox, 0, m_zoomable, 0);
				disconnect(m_window->actionZoomIn, 0, m_zoomable, 0);
				disconnect(m_window->actionZoomOut, 0, m_zoomable, 0);
				disconnect(m_window->actionBestFit, 0, m_zoomable, 0);
            }
            return QObject::eventFilter(object, event);
        }
        MainWindow *m_window;
        ZoomableView *m_zoomable;
    };
    view->installEventFilter(new ZoomFilter(view, this));
}

template <class T>
void MainWindow::setupPrintableView(T *view)
{
    static_assert(std::is_base_of_v<QObject, T>, "Missing QObject");
    struct PrintFilter : public QObject
    {
        PrintFilter(T *printable, MainWindow *window)
            : QObject(printable), m_window(window), m_printable(printable)
        {
        }
        bool eventFilter(QObject *object, QEvent *event) override
        {
            auto setEnabled = [this](bool state) {
				m_window->actionPrint->setEnabled(state);
				m_window->actionPrintPreview->setEnabled(state);
            };
            if (event->type() == QEvent::Show) {
                setEnabled(true);
				connect(m_window->actionPrint, &QAction::triggered,
                        m_printable, [this] { m_printable->print(); });
				connect(m_window->actionPrintPreview, &QAction::triggered,
                        m_printable, [this] { m_printable->printPreview(); });
            } else if (event->type() == QEvent::Hide) {
                setEnabled(false);
				disconnect(m_window->actionPrint, 0, m_printable, 0);
				disconnect(m_window->actionPrintPreview, 0, m_printable, 0);
            }
            return QObject::eventFilter(object, event);
        }
        MainWindow *m_window;
        T *m_printable;
    };

    view->installEventFilter(new PrintFilter(view, this));
}

template <class T>
void MainWindow::setupExportableView(T *view)
{
    struct ExportFilter : public QObject
    {
        ExportFilter(T *exportable, MainWindow *window)
            : QObject(exportable), m_window(window), m_exportable(exportable)
        {
        }

        bool eventFilter(QObject *object, QEvent *event) override
        {
            if (event->type() == QEvent::Show) {
				m_window->actionExportAs->setEnabled(true);
				connect(m_window->actionExportAs, &QAction::triggered,
                        m_exportable, [this] { m_exportable->exportAs(); });
            } else if (event->type() == QEvent::Hide) {
				m_window->actionExportAs->setEnabled(false);
				disconnect(m_window->actionExportAs, 0, m_exportable, 0);
            }

            return QObject::eventFilter(object, event);
        }

        MainWindow *m_window;
        T *m_exportable;
    };
    view->installEventFilter(new ExportFilter(view, this));
}

template <class T>
void MainWindow::setupSearchable(QObject *view, T *model)
{
    struct SearchFilter : public QObject
    {
        SearchFilter(T *searchable, MainWindow *window)
            : QObject(searchable), m_window(window), m_searchable(searchable)
        {
        }

        bool eventFilter(QObject *object, QEvent *event) override
        {
            if (event->type() == QEvent::Show) {
                m_window->m_searchBar->setHidden(false);
                m_window->m_searchBar->setText(
                    m_searchable->filterRegExp().pattern());
                connect(m_window->m_searchBar, &QLineEdit::editingFinished,
                        object, [this] {
                            m_searchable->setFilterRegExp(
                                m_window->m_searchBar->text());
                        });
            } else if (event->type() == QEvent::Hide) {
                m_window->m_searchBar->setHidden(true);
                disconnect(m_window->m_searchBar, 0, object, 0);
            }

            return QObject::eventFilter(object, event);
        }

        MainWindow *m_window;
        T *m_searchable;
    };
    view->installEventFilter(new SearchFilter(model, this));
}

/// Specialization to find the fault tree container of a gate.
///
/// @param[in] gate  The gate belonging exactly to one fault tree.
///
/// @returns The fault tree container with the given gate.
template <>
mef::FaultTree *MainWindow::getFaultTree(mef::Gate *gate)
{
    /// @todo Duplicate code from EventDialog.
    auto it = boost::find_if(m_model->table<mef::FaultTree>(),
                             [&gate](const mef::FaultTree &faultTree) {
                                 return faultTree.gates().count(gate->name());
                             });
    GUI_ASSERT(it != m_model->table<mef::FaultTree>().end(), nullptr);
    return &*it;
}

template <class T>
void MainWindow::removeEvent(T *event, mef::FaultTree *faultTree)
{
    m_undoStack->push(
        new model::Model::RemoveEvent<T>(event, m_guiModel.get(), faultTree));
}

/// Specialization to deal with complexities of gate/fault-tree removal.
template <>
void MainWindow::removeEvent(model::Gate *event, mef::FaultTree *faultTree)
{
    GUI_ASSERT(faultTree->top_events().empty() == false, );
    GUI_ASSERT(faultTree->gates().empty() == false, );
    if (faultTree->top_events().front() != event->data()) {
        m_undoStack->push(new model::Model::RemoveEvent<model::Gate>(
            event, m_guiModel.get(), faultTree));
        return;
    }
    QString faultTreeName = QString::fromStdString(faultTree->name());
    if (faultTree->gates().size() > 1) {
        QMessageBox::information(
            this,
            //: The container w/ dependents still in the model.
            _("Dependency Container Removal"),
            _("Fault tree '%1' with root '%2' is not removable because"
              " it has dependent non-root gates."
              " Remove the gates from the fault tree"
              " before this operation.")
                .arg(faultTreeName, event->id()));
        return;
    }
    m_undoStack->beginMacro(_("Remove fault tree '%1' with root '%2'")
                                .arg(faultTreeName, event->id()));
    m_undoStack->push(new model::Model::RemoveEvent<model::Gate>(
        event, m_guiModel.get(), faultTree));
    m_undoStack->push(
        new model::Model::RemoveFaultTree(faultTree, m_guiModel.get()));
    m_undoStack->endMacro();
}

template <class T>
void MainWindow::setupRemovable(QAbstractItemView *view)
{
    struct RemoveFilter : public QObject
    {
        RemoveFilter(QAbstractItemView *removable, MainWindow *window)
            : QObject(removable), m_window(window), m_removable(removable)
        {
        }

        void react(const QModelIndexList &indexes)
        {
			m_window->actionRemoveElement->setEnabled(
                !(indexes.empty() || indexes.front().parent().isValid()));
        }

        bool eventFilter(QObject *object, QEvent *event) override
        {
            if (event->type() == QEvent::Show) {
                react(m_removable->selectionModel()->selectedIndexes());
                connect(
                    m_removable->model(), &QAbstractItemModel::modelReset,
                    m_removable, [this] {
                        react(m_removable->selectionModel()->selectedIndexes());
                    });
                connect(m_removable->selectionModel(),
                        &QItemSelectionModel::selectionChanged,
						m_window->actionRemoveElement,
                        [this](const QItemSelection &selected) {
                            react(selected.indexes());
                        });
                connect(
					m_window->actionRemoveElement, &QAction::triggered,
                    m_removable, [this] {
                        auto currentIndexes =
                            m_removable->selectionModel()->selectedIndexes();
                        GUI_ASSERT(currentIndexes.empty() == false, );
                        auto index = currentIndexes.front();
                        GUI_ASSERT(index.parent().isValid() == false, );
                        auto *element = static_cast<T *>(
                            index.data(Qt::UserRole).value<void *>());
                        GUI_ASSERT(element, );
                        auto parents =
                            m_window->m_guiModel->parents(element->data());
                        if (!parents.empty()) {
                            QMessageBox::information(
                                m_window,
                                //: The event w/ dependents in the model.
                                _("Dependency Event Removal"),
                                _("Event '%1' is not removable because"
                                  " it has dependents."
                                  " Remove the event from the dependents"
                                  " before this operation.")
                                    .arg(element->id()));
                            return;
                        }
                        m_window->removeEvent(
                            element, m_window->getFaultTree(element->data()));
                    });
            } else if (event->type() == QEvent::Hide) {
				m_window->actionRemoveElement->setEnabled(false);
				disconnect(m_window->actionRemoveElement, 0, m_removable,
                           0);
            }

            return QObject::eventFilter(object, event);
        }

        MainWindow *m_window;
        QAbstractItemView *m_removable;
    };
    view->installEventFilter(new RemoveFilter(view, this));
}

/// Specialization to construct formula out of event editor data.
///
/// @param[in] dialog  The valid event dialog with data for a gate formula.
///
/// @returns A new formula with arguments from the event dialog.
template <>
std::unique_ptr<mef::Formula> MainWindow::extract(const EventDialog &dialog)
{
    auto getEvent = [this](const std::string &arg) -> mef::Formula::ArgEvent {
        try {
            return m_model->GetEvent(arg);
        } catch (const mef::UndefinedElement &) {
            auto argEvent = std::make_unique<mef::BasicEvent>(arg);
            argEvent->AddAttribute({"flavor", "undeveloped", ""});
            auto *address = argEvent.get();
            /// @todo Add into the parent undo.
            m_undoStack->push(new model::Model::AddEvent<model::BasicEvent>(
                std::move(argEvent), m_guiModel.get()));
            return address;
        }
    };

    mef::Formula::ArgSet arg_set;
    for (const std::string &arg : dialog.arguments())
        arg_set.Add(getEvent(arg));

    mef::Connective connective = dialog.connective();
    auto minNumber = [&connective, &dialog]() -> std::optional<int> {
        if (connective == mef::kAtleast)
            dialog.minNumber();
        return {};
    }();
    return std::make_unique<mef::Formula>(connective, std::move(arg_set),
                                          minNumber);
}

/// Specialization to construct basic event out of event editor data.
template <>
std::unique_ptr<mef::BasicEvent> MainWindow::extract(const EventDialog &dialog)
{
    auto basicEvent =
        std::make_unique<mef::BasicEvent>(dialog.name().toStdString());
    basicEvent->label(dialog.label().toStdString());
    switch (dialog.currentType()) {
    case EventDialog::BasicEvent:
        break;
    case EventDialog::Undeveloped:
        basicEvent->AddAttribute({"flavor", "undeveloped", ""});
        break;
    default:
        GUI_ASSERT(false && "unexpected event type", nullptr);
    }
    if (auto p_expression = dialog.expression()) {
        basicEvent->expression(p_expression.get());
        m_model->Add(std::move(p_expression));
    }
    return basicEvent;
}

/// Specialization to construct house event out of event editor data.
template <>
std::unique_ptr<mef::HouseEvent> MainWindow::extract(const EventDialog &dialog)
{
    GUI_ASSERT(dialog.currentType() == EventDialog::HouseEvent, nullptr);
    auto houseEvent =
        std::make_unique<mef::HouseEvent>(dialog.name().toStdString());
    houseEvent->label(dialog.label().toStdString());
    houseEvent->state(dialog.booleanConstant());
    return houseEvent;
}

/// Specialization to construct gate out of event editor data.
template <>
std::unique_ptr<mef::Gate> MainWindow::extract(const EventDialog &dialog)
{
    GUI_ASSERT(dialog.currentType() == EventDialog::Gate, nullptr);
    auto gate = std::make_unique<mef::Gate>(dialog.name().toStdString());
    gate->label(dialog.label().toStdString());
    gate->formula(extract<mef::Formula>(dialog));
    return gate;
}

void MainWindow::addElement()
{
    EventDialog dialog(m_model.get(), this);
    if (dialog.exec() == QDialog::Rejected)
        return;
    switch (dialog.currentType()) {
    case EventDialog::HouseEvent:
        m_undoStack->push(new model::Model::AddEvent<model::HouseEvent>(
            extract<mef::HouseEvent>(dialog), m_guiModel.get()));
        break;
    case EventDialog::BasicEvent:
    case EventDialog::Undeveloped:
        m_undoStack->push(new model::Model::AddEvent<model::BasicEvent>(
            extract<mef::BasicEvent>(dialog), m_guiModel.get()));
        break;
    case EventDialog::Gate: {
        m_undoStack->beginMacro(
            //: Addition of a fault by defining its root event first.
            _("Add fault tree '%1' with gate '%2'")
                .arg(QString::fromStdString(dialog.faultTree()),
                     dialog.name()));
        auto faultTree = std::make_unique<mef::FaultTree>(dialog.faultTree());
        auto *faultTreeAddress = faultTree.get();
        m_undoStack->push(new model::Model::AddFaultTree(std::move(faultTree),
                                                         m_guiModel.get()));
        m_undoStack->push(new model::Model::AddEvent<model::Gate>(
            extract<mef::Gate>(dialog), m_guiModel.get(), faultTreeAddress));
        faultTreeAddress->CollectTopEvents();
        m_undoStack->endMacro();
    } break;
    default:
        GUI_ASSERT(false && "unexpected event type", );
    }
}

mef::FaultTree *MainWindow::getFaultTree(const EventDialog &dialog)
{
    if (dialog.faultTree().empty())
        return nullptr;
    auto it = m_model->table<mef::FaultTree>().find(dialog.faultTree());
    GUI_ASSERT(it != m_model->table<mef::FaultTree>().end(), nullptr);
    return &*it;
}

template <class T>
void MainWindow::editElement(EventDialog *dialog, model::Element *element)
{
    if (dialog->name() != element->id()) {
        m_undoStack->push(new model::Element::SetId<T>(
            static_cast<T *>(element), dialog->name(), m_model.get(),
            getFaultTree(*dialog)));
    }
    if (dialog->label() != element->label())
        m_undoStack->push(
            new model::Element::SetLabel(element, dialog->label()));
}

void MainWindow::editElement(EventDialog *dialog, model::BasicEvent *element)
{
    editElement<model::BasicEvent>(dialog, element);
    switch (dialog->currentType()) {
    case EventDialog::HouseEvent:
        m_undoStack->push(new model::Model::ChangeEventType<model::BasicEvent,
                                                            model::HouseEvent>(
            element, extract<mef::HouseEvent>(*dialog), m_guiModel.get(),
            getFaultTree(*dialog)));
        return;
    case EventDialog::BasicEvent:
    case EventDialog::Undeveloped:
        break;
    case EventDialog::Gate:
        m_undoStack->push(
            new model::Model::ChangeEventType<model::BasicEvent, model::Gate>(
                element, extract<mef::Gate>(*dialog), m_guiModel.get(),
                getFaultTree(*dialog)));
        return;
    default:
        GUI_ASSERT(false && "Unexpected event type", );
    }
    std::unique_ptr<mef::Expression> expression = dialog->expression();
    auto isEqual = [](mef::Expression *lhs, mef::Expression *rhs) {
        if (lhs == rhs) // Assumes immutable expressions.
            return true;
        if (!lhs || !rhs)
            return false;

        auto *const_lhs = dynamic_cast<mef::ConstantExpression *>(lhs);
        auto *const_rhs = dynamic_cast<mef::ConstantExpression *>(rhs);
        if (const_lhs && const_rhs && const_lhs->value() == const_rhs->value())
            return true;

        auto *exp_lhs = dynamic_cast<mef::Exponential *>(lhs);
        auto *exp_rhs = dynamic_cast<mef::Exponential *>(rhs);
        if (exp_lhs && exp_rhs
            && exp_lhs->args().front()->value()
                   == exp_rhs->args().front()->value())
            return true;

        return false;
    };

    if (!isEqual(expression.get(), element->expression())) {
        m_undoStack->push(
            new model::BasicEvent::SetExpression(element, expression.get()));
        m_model->Add(std::move(expression));
    }

    auto flavorToType = [](model::BasicEvent::Flavor flavor) {
        switch (flavor) {
        case model::BasicEvent::Basic:
            return EventDialog::BasicEvent;
        case model::BasicEvent::Undeveloped:
            return EventDialog::Undeveloped;
        }
        assert(false);
    };

    if (dialog->currentType() != flavorToType(element->flavor())) {
        m_undoStack->push([&dialog, &element]() -> QUndoCommand * {
            switch (dialog->currentType()) {
            case EventDialog::BasicEvent:
                return new model::BasicEvent::SetFlavor(
                    element, model::BasicEvent::Basic);
            case EventDialog::Undeveloped:
                return new model::BasicEvent::SetFlavor(
                    element, model::BasicEvent::Undeveloped);
            default:
                GUI_ASSERT(false && "Unexpected event type", nullptr);
            }
        }());
    }
}

void MainWindow::editElement(EventDialog *dialog, model::HouseEvent *element)
{
    editElement<model::HouseEvent>(dialog, element);
    switch (dialog->currentType()) {
    case EventDialog::HouseEvent:
        break;
    case EventDialog::BasicEvent:
    case EventDialog::Undeveloped:
        m_undoStack->push(new model::Model::ChangeEventType<model::HouseEvent,
                                                            model::BasicEvent>(
            element, extract<mef::BasicEvent>(*dialog), m_guiModel.get(),
            getFaultTree(*dialog)));
        return;
    case EventDialog::Gate:
        m_undoStack->push(
            new model::Model::ChangeEventType<model::HouseEvent, model::Gate>(
                element, extract<mef::Gate>(*dialog), m_guiModel.get(),
                getFaultTree(*dialog)));
        return;
    default:
        GUI_ASSERT(false && "Unexpected event type", );
    }
    if (dialog->booleanConstant() != element->state())
        m_undoStack->push(new model::HouseEvent::SetState(
            element, dialog->booleanConstant()));
}

void MainWindow::editElement(EventDialog *dialog, model::Gate *element)
{
    editElement<model::Gate>(dialog, element);
    switch (dialog->currentType()) {
    case EventDialog::HouseEvent:
        m_undoStack->push(
            new model::Model::ChangeEventType<model::Gate, model::HouseEvent>(
                element, extract<mef::HouseEvent>(*dialog), m_guiModel.get(),
                getFaultTree(*dialog)));
        return;
    case EventDialog::BasicEvent:
    case EventDialog::Undeveloped:
        m_undoStack->push(
            new model::Model::ChangeEventType<model::Gate, model::BasicEvent>(
                element, extract<mef::BasicEvent>(*dialog), m_guiModel.get(),
                getFaultTree(*dialog)));
        return;
    case EventDialog::Gate:
        break;
    default:
        GUI_ASSERT(false && "Unexpected event type", );
    }

    bool formulaChanged = [&dialog, &element] {
        if (dialog->connective() != element->type())
            return true;
        if (element->minNumber()
            && dialog->minNumber() != *element->minNumber())
            return true;
        std::vector<std::string> dialogArgs = dialog->arguments();
        if (element->numArgs() != dialogArgs.size())
            return true;
        auto it = dialogArgs.begin();
        for (const mef::Formula::Arg &arg : element->args()) {
            if (*it != ext::as<const mef::Event *>(arg.event)->id())
                return true;
            ++it;
        }
        return false;
    }();
    if (formulaChanged)
        m_undoStack->push(new model::Gate::SetFormula(
            element, extract<mef::Formula>(*dialog)));
}

template <class ContainerModel, typename... Ts>
QTableView *MainWindow::constructTableView(QWidget *parent, Ts &&... modelArgs)
{
    auto *table = new QTableView(parent);
    auto *tableModel =
        new ContainerModel(std::forward<Ts>(modelArgs)..., table);
    auto *proxyModel = new model::SortFilterProxyModel(table);
    proxyModel->setSourceModel(tableModel);
    table->setModel(proxyModel);
    table->setWordWrap(false);
    table->horizontalHeader()->setSortIndicatorShown(true);
    table->resizeColumnsToContents();
    table->setSortingEnabled(true);
    setupSearchable(table, proxyModel);
    return table;
}

template <class ContainerModel>
QAbstractItemView *MainWindow::constructElementTable(model::Model *guiModel,
                                                     QWidget *parent)
{
    QTableView *table = constructTableView<ContainerModel>(parent, guiModel);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    setupRemovable<typename ContainerModel::ItemModel>(table);
    connect(table, &QAbstractItemView::activated, this,
            [this](const QModelIndex &index) {
                GUI_ASSERT(index.isValid(), );
                EventDialog dialog(m_model.get(), this);
                auto *item = static_cast<typename ContainerModel::ItemModel *>(
                    index.data(Qt::UserRole).value<void *>());
                GUI_ASSERT(item, );
                dialog.setupData(*item);
                if (dialog.exec() == QDialog::Accepted)
                    editElement(&dialog, item);
            });
    return table;
}

/// Specialization to show gates as trees in tables.
template <>
QAbstractItemView *MainWindow::constructElementTable<model::GateContainerModel>(
    model::Model *guiModel, QWidget *parent)
{
    auto *tree = new QTreeView(parent);
    auto *tableModel = new model::GateContainerModel(guiModel, tree);
    auto *proxyModel = new model::GateSortFilterProxyModel(tree);
    proxyModel->setSourceModel(tableModel);
    tree->setModel(proxyModel);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setWordWrap(false);
    tree->header()->setSortIndicatorShown(true);
    tree->header()->setDefaultAlignment(Qt::AlignCenter);
    tree->resizeColumnToContents(0);
    tree->setColumnWidth(0, 2 * tree->columnWidth(0));
    tree->setAlternatingRowColors(true);
    tree->setSortingEnabled(true);

    setupSearchable(tree, proxyModel);
    setupRemovable<model::Gate>(tree);
    connect(tree, &QAbstractItemView::activated, this,
            [this](const QModelIndex &index) {
                GUI_ASSERT(index.isValid(), );
                if (index.parent().isValid())
                    return;
                EventDialog dialog(m_model.get(), this);
                auto *item = static_cast<model::Gate *>(
                    index.data(Qt::UserRole).value<void *>());
                GUI_ASSERT(item, );
                dialog.setupData(*item);
                if (dialog.exec() == QDialog::Accepted)
                    editElement(&dialog, item);
            });
    return tree;
}

void MainWindow::resetModelTree()
{
//	while (tabWidget->count()) {
//		auto *widget = tabWidget->widget(0);
//		tabWidget->removeTab(0);
//        delete widget;
//    }
    m_guiModel = std::make_unique<model::Model>(m_model.get());
	auto *oldModel = modelTree->model();
	modelTree->setModel(new ModelTree(m_guiModel.get(), this));
    delete oldModel;

    connect(m_guiModel.get(), &model::Model::modelNameChanged, this, [this] {
        setWindowTitle(QStringLiteral("%1[*]").arg(getModelNameForTitle()));
    });
}

bool MainWindow::activateTab(const QString& title) {
	return false;
//	for (int i=0; i < tabWidget->count(); i++) {
//		if (tabWidget->tabText(i) == title) {
//			tabWidget->setCurrentIndex(i);
//			return true;
//		}
//	}
//	return false;
}

void MainWindow::activateModelTree(const QModelIndex &index)
{
    GUI_ASSERT(index.isValid(), );
    if (index.parent().isValid() == false) {
        switch (static_cast<ModelTree::Row>(index.row())) {
        case ModelTree::Row::Gates: {
			const auto title = _("Gates");
			if (activateTab(title))
				return;
			auto* dock = new KDDockWidgets::DockWidget(uniqueName());
            auto *table = constructElementTable<model::GateContainerModel>(
				m_guiModel.get(), dock);
            //: The tab for the table of gates.
			dock->setTitle(title);
			dock->setWidget(table);
			addDockWidget(dock, KDDockWidgets::Location_OnRight);
            return;
        }
        case ModelTree::Row::BasicEvents: {
			const auto title = _("Basic Events");
			if (activateTab(title))
				return;
			auto* dock = new KDDockWidgets::DockWidget(uniqueName());
            auto *table =
                constructElementTable<model::BasicEventContainerModel>(
					m_guiModel.get(), dock);
            //: The tab for the table of basic events.	
			dock->setTitle(title);
			dock->setWidget(table);
			addDockWidget(dock, KDDockWidgets::Location_OnRight);
            return;
        }
        case ModelTree::Row::HouseEvents: {
			const auto title = _("House Events");
			if (activateTab(title))
				return;
			auto* dock = new KDDockWidgets::DockWidget(uniqueName());
            auto *table =
                constructElementTable<model::HouseEventContainerModel>(
					m_guiModel.get(), dock);
            //: The tab for the table of house events.
			dock->setTitle(title);
			dock->setWidget(table);
			addDockWidget(dock, KDDockWidgets::Location_OnRight);
            return;
        }
        case ModelTree::Row::FaultTrees:
            return;
        }
        GUI_ASSERT(false, );
    }
    GUI_ASSERT(index.parent().parent().isValid() == false, );
    GUI_ASSERT(index.parent().row()
                   == static_cast<int>(ModelTree::Row::FaultTrees), );
    auto faultTree =
        static_cast<mef::FaultTree *>(index.data(Qt::UserRole).value<void *>());
    GUI_ASSERT(faultTree, );
    activateFaultTreeDiagram(faultTree);
}

void MainWindow::activateReportTree(const QModelIndex &index)
{
    GUI_ASSERT(m_analysis, );
    GUI_ASSERT(index.isValid(), );
    QModelIndex parentIndex = index.parent();
    if (!parentIndex.isValid())
        return;
    GUI_ASSERT(parentIndex.parent().isValid() == false, );
    QString name = parentIndex.data(Qt::DisplayRole).toString();
    GUI_ASSERT(parentIndex.row() < m_analysis->results().size(), );
    const core::RiskAnalysis::Result &result =
        m_analysis->results()[parentIndex.row()];

    QWidget *widget = nullptr;
    switch (static_cast<ReportTree::Row>(index.row())) {
    case ReportTree::Row::Products: {
		const auto title = _("Products: %1").arg(name);
		if (activateTab(title))
			return;
        bool withProbability = result.probability_analysis != nullptr;
		auto* dock = new KDDockWidgets::DockWidget(uniqueName());
		auto* table = constructTableView<model::ProductTableModel>(
			dock, result.fault_tree_analysis->products(), withProbability);
		dock->setTitle(title);
		dock->setWidget(table);
		addDockWidget(dock, KDDockWidgets::Location_OnRight);
        table->sortByColumn(withProbability ? 2 : 1, withProbability
                                                         ? Qt::DescendingOrder
                                                         : Qt::AscendingOrder);
        table->setSortingEnabled(true);
        widget = table;
        break;
    }
    case ReportTree::Row::Probability:
        break;
    case ReportTree::Row::Importance: {
		const auto title = _("Importance: %1").arg(name);
		if (activateTab(title))
			return;
		auto* dock = new KDDockWidgets::DockWidget(uniqueName());
        widget = constructTableView<model::ImportanceTableModel>(
			dock, &result.importance_analysis->importance());
		dock->setTitle(title);
		dock->setWidget(widget);
		addDockWidget(dock, KDDockWidgets::Location_OnRight);
        break;
    }
    default:
        GUI_ASSERT(false && "Unexpected analysis report data", );
    }

    if (!widget)
        return;
//	connect(reportTree->model(), &QObject::destroyed, widget,
//			[this, widget] { closeTab(tabWidget->indexOf(widget)); });
}

void MainWindow::activateFaultTreeDiagram(mef::FaultTree *faultTree)
{
    GUI_ASSERT(faultTree, );
    GUI_ASSERT(faultTree->top_events().size() == 1, );

	const auto title = _("Fault Tree: %1").arg(QString::fromStdString(faultTree->name()));
	if (activateTab(title))
		return;

    auto *topGate = faultTree->top_events().front();
	auto* dock = new KDDockWidgets::DockWidget(uniqueName());
	auto *view = new DiagramView(dock);
    auto *scene = new diagram::DiagramScene(
        m_guiModel->gates().find(topGate)->get(), m_guiModel.get(), view);
    view->setScene(scene);
    view->setViewport(new QGLWidget(QGLFormat(QGL::SampleBuffers)));
    view->setRenderHints(QPainter::Antialiasing
                         | QPainter::SmoothPixmapTransform);
    view->setAlignment(Qt::AlignTop);
    view->ensureVisible(0, 0, 0, 0);
    setupZoomableView(view);
    setupPrintableView(view);
    setupExportableView(view);

	//: The dock for a fault tree diagram.
	dock->setTitle(title);
	dock->setWidget(view);
	addDockWidget(dock, KDDockWidgets::Location_OnRight);

    connect(scene, &diagram::DiagramScene::activated, this,
            [this](model::Element *element) {
                EventDialog dialog(m_model.get(), this);
                auto action = [this, &dialog](auto *target) {
                    dialog.setupData(*target);
                    if (dialog.exec() == QDialog::Accepted) {
                        editElement(&dialog, target);
                    }
                };
                /// @todo Redesign/remove the RAII!
                if (auto *basic = dynamic_cast<model::BasicEvent *>(element)) {
                    action(basic);
                } else if (auto *gate = dynamic_cast<model::Gate *>(element)) {
                    action(gate);
                } else {
                    auto *house = dynamic_cast<model::HouseEvent *>(element);
                    GUI_ASSERT(house, );
                    action(house);
                }
            });
    connect(m_guiModel.get(),
            qOverload<mef::FaultTree *>(&model::Model::removed), view,
            [this, faultTree, view](mef::FaultTree *removedTree) {
//                if (removedTree == faultTree)
//					closeTab(tabWidget->indexOf(view));
            });
}

void MainWindow::resetReportTree(std::unique_ptr<core::RiskAnalysis> analysis)
{
	actionExportReportAs->setEnabled(static_cast<bool>(analysis));

	auto *oldModel = reportTree->model();
	reportTree->setModel(
        analysis ? new ReportTree(&analysis->results(), this) : nullptr);
    delete oldModel;
    m_analysis = std::move(analysis);
}

} // namespace scram::gui
