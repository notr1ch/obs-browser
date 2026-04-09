#include "browser-panel-client.hpp"
#include <util/dstr.h>

#include <QUrl>
#include <QDesktopServices>
#include <QApplication>
#include <QMenu>
#include <QThread>
#include <QMessageBox>
#include <QInputDialog>
#include <QRegularExpression>
#include <QLabel>
#include <QClipboard>

#include <obs-module.h>
#ifdef _WIN32
#include <windows.h>
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#endif

#define MENU_ITEM_DEVTOOLS MENU_ID_CUSTOM_FIRST
#define MENU_ITEM_MUTE MENU_ID_CUSTOM_FIRST + 1
#define MENU_ITEM_ZOOM_IN MENU_ID_CUSTOM_FIRST + 2
#define MENU_ITEM_ZOOM_RESET MENU_ID_CUSTOM_FIRST + 3
#define MENU_ITEM_ZOOM_OUT MENU_ID_CUSTOM_FIRST + 4
#define MENU_ITEM_COPY_URL MENU_ID_CUSTOM_FIRST + 5

static int zoomLvls[] = {25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400};

bool zoomPage(CefRefPtr<CefBrowserHost> host, int direction)
{
	if (!host || direction < -1 || direction > 1)
		return false;

	if (direction == 0) {
		host->SetZoomLevel(0);
		return true;
	}

	int currentZoomPercent = (int)round(pow(1.2, host->GetZoomLevel()) * 100.0);
	int zoomCount = sizeof(zoomLvls) / sizeof(zoomLvls[0]);
	int zoomIdx = 0;

	while (zoomIdx < zoomCount) {
		if (zoomLvls[zoomIdx] == currentZoomPercent)
			break;
		zoomIdx++;
	}
	if (zoomIdx == zoomCount)
		return false;

	int newZoomIdx = zoomIdx;
	if (direction == -1 && zoomIdx > 0)
		newZoomIdx -= 1;
	else if (direction == 1 && zoomIdx < zoomCount - 1)
		newZoomIdx += 1;

	if (newZoomIdx != zoomIdx) {
		host->SetZoomLevel(log(zoomLvls[newZoomIdx] / 100.0) / log(1.2));
		return true;
	}
	return false;
}

// IMPORTANT DESIGN NOTE: All main thread invokeMethods in the CEF callbacks below must go through QCoreApplication
// first to push it onto the UI thread, otherwise there is a race condition where the widget exists and gets destroyed
// in between the check and invoke, causing a crash in invoke (once it's in the event queue it's safe, but not before).

CefRefPtr<CefLoadHandler> QCefBrowserClient::GetLoadHandler()
{
	return this;
}

CefRefPtr<CefDisplayHandler> QCefBrowserClient::GetDisplayHandler()
{
	return this;
}

CefRefPtr<CefRequestHandler> QCefBrowserClient::GetRequestHandler()
{
	return this;
}

CefRefPtr<CefLifeSpanHandler> QCefBrowserClient::GetLifeSpanHandler()
{
	return this;
}

CefRefPtr<CefFocusHandler> QCefBrowserClient::GetFocusHandler()
{
	return this;
}

CefRefPtr<CefContextMenuHandler> QCefBrowserClient::GetContextMenuHandler()
{
	return this;
}

CefRefPtr<CefKeyboardHandler> QCefBrowserClient::GetKeyboardHandler()
{
	return this;
}

CefRefPtr<CefJSDialogHandler> QCefBrowserClient::GetJSDialogHandler()
{
	return this;
}

void QCefBrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title)
{
	CefRefPtr<CefBrowser> ours = session->getBrowser();

	if (ours && ours->IsSame(browser)) {

		std::string str_title = title;
		QString qt_title = QString::fromUtf8(str_title.c_str());

		auto weakSession = std::weak_ptr<BrowserSession>(session);
		QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), [weakSession, qt_title]() {
			auto s = weakSession.lock();
			if (!s)
				return;
			QCefWidgetInternal *w = s->getWidget();
			if (w)
				QMetaObject::invokeMethod(w, "titleChanged", Q_ARG(QString, qt_title));
		});
	} else {
		// Popup title — post everything to the main UI thread to avoid cross-thread issues
		std::string str_title = title.ToString();
		auto weakSession = std::weak_ptr<BrowserSession>(session);
		CefRefPtr<CefBrowserHost> host = browser->GetHost();

		QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), [weakSession, host, str_title]() {
			std::string finalTitle = str_title;

			if (str_title == "DevTools") {
				auto s = weakSession.lock();
				if (s) {
					QCefWidgetInternal *w = s->getWidget();
					if (w && w->parentWidget()) {
						finalTitle = QString(obs_module_text("DevTools"))
								     .arg(w->parentWidget()->windowTitle())
								     .toUtf8()
								     .constData();
					}
				}
			}

#if defined(_WIN32)
			CefWindowHandle handl = host->GetWindowHandle();
			std::wstring wTitle(CefString(finalTitle).ToWString());
			SetWindowTextW((HWND)handl, wTitle.c_str());
#elif defined(__linux__)
			CefWindowHandle handl = host->GetWindowHandle();
			XStoreName(cef_get_xdisplay(), handl, finalTitle.c_str());
#endif
		});
	}
}

bool QCefBrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
				       CefRefPtr<CefRequest> request, bool, bool)
{
	std::string str_url = request->GetURL();

	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	for (size_t i = forced_popups.size(); i > 0; i--) {
		PopupWhitelistInfo &info = forced_popups[i - 1];

		if (!info.obj) {
			forced_popups.erase(forced_popups.begin() + (i - 1));
			continue;
		}

		if (astrcmpi(info.url.c_str(), str_url.c_str()) == 0) {
			// Open tab popup URLs in user's actual browser
			QUrl url = QUrl(str_url.c_str(), QUrl::TolerantMode);
			QDesktopServices::openUrl(url);
			browser->GoBack();
			return true;
		}
	}

	auto weakSession = std::weak_ptr<BrowserSession>(session);
	QString qt_url = QString::fromUtf8(str_url.c_str());

	QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), [weakSession, qt_url]() {
		auto s = weakSession.lock();
		if (!s)
			return;
		QCefWidgetInternal *w = s->getWidget();
		if (w)
			QMetaObject::invokeMethod(w, "urlChanged", Q_ARG(QString, qt_url));
	});

	return false;
}

bool QCefBrowserClient::OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &target_url,
					 CefRequestHandler::WindowOpenDisposition, bool)
{
	std::string str_url = target_url;

	// Open tab popup URLs in user's actual browser
	QUrl url = QUrl(str_url.c_str(), QUrl::TolerantMode);
	QDesktopServices::openUrl(url);
	return true;
}

void QCefBrowserClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				    CefLoadHandler::ErrorCode errorCode, const CefString &errorText,
				    const CefString &failedUrl)
{
	UNUSED_PARAMETER(browser);
	if (errorCode == ERR_ABORTED)
		return;

	struct dstr html;
	char *path = obs_module_file("error.html");
	char *errorPage = os_quick_read_utf8_file(path);

	dstr_init_copy(&html, errorPage);

	dstr_replace(&html, "%%ERROR_URL%%", failedUrl.ToString().c_str());

	dstr_replace(&html, "Error.Title", obs_module_text("Error.Title"));
	dstr_replace(&html, "Error.Description", obs_module_text("Error.Description"));
	dstr_replace(&html, "Error.Retry", obs_module_text("Error.Retry"));
	const char *translError;
	std::string errorKey = "ErrorCode." + errorText.ToString();
	if (obs_module_get_string(errorKey.c_str(), (const char **)&translError)) {
		dstr_replace(&html, "%%ERROR_CODE%%", translError);
	} else {
		dstr_replace(&html, "%%ERROR_CODE%%", errorText.ToString().c_str());
	}

	frame->LoadURL("data:text/html;base64," +
		       CefURIEncode(CefBase64Encode(html.array, html.len), false).ToString());

	dstr_free(&html);
	bfree(path);
	bfree(errorPage);
}

bool QCefBrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
#if CHROME_VERSION_BUILD >= 6834
				      int,
#endif
				      const CefString &target_url, const CefString &,
				      CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures &,
				      CefWindowInfo &windowInfo, CefRefPtr<CefClient> &, CefBrowserSettings &,
				      CefRefPtr<CefDictionaryValue> &, bool *)
{
	if (allowAllPopups) {
#ifdef _WIN32
		QCefWidgetInternal *w = session->getWidget();
		if (w) {
			HWND hwnd = (HWND)w->effectiveWinId();
			windowInfo.parent_window = hwnd;
		}
#else
		UNUSED_PARAMETER(windowInfo);
#endif
		return false;
	}

	std::string str_url = target_url;

	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	for (size_t i = popup_whitelist.size(); i > 0; i--) {
		PopupWhitelistInfo &info = popup_whitelist[i - 1];

		if (!info.obj) {
			popup_whitelist.erase(popup_whitelist.begin() + (i - 1));
			continue;
		}

		if (astrcmpi(info.url.c_str(), str_url.c_str()) == 0) {
#ifdef _WIN32
			QCefWidgetInternal *w = session->getWidget();
			if (w) {
				HWND hwnd = (HWND)w->effectiveWinId();
				windowInfo.parent_window = hwnd;
			}
#endif
			return false;
		}
	}

	// Open popup URLs in user's actual browser
	QUrl url = QUrl(str_url.c_str(), QUrl::TolerantMode);
	QDesktopServices::openUrl(url);
	return true;
}

bool QCefBrowserClient::OnSetFocus(CefRefPtr<CefBrowser>, CefFocusHandler::FocusSource source)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0) && QT_VERSION < QT_VERSION_CHECK(6, 11, 1)
	// Workaround for browser docks flashing/hanging at startup with Qt 6.8.x, introduced
	// by commit https://code.qt.io/cgit/qt/qt5.git/commit/?id=bab1fecd556ea561c4a89686293116741acfa1b4.
	// Refer to https://bugreports.qt.io/browse/QTBUG-136165.
	UNUSED_PARAMETER(source);
	return false;
#else
	// Don't steal focus when the webpage navigates. This is especially
	// obvious on startup when the user has many browser docks defined,
	// as each one will steal focus one by one, resulting in poor UX.
	switch (source) {
	case FOCUS_SOURCE_NAVIGATION:
		return true;
	default:
		return false;
	}
#endif
}

void QCefBrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
					    CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> model)
{
	if (model->IsVisible(MENU_ID_BACK) &&
	    (!model->IsVisible(MENU_ID_RELOAD) && !model->IsVisible(MENU_ID_RELOAD_NOCACHE))) {
		model->InsertItemAt(2, MENU_ID_RELOAD_NOCACHE, QObject::tr("RefreshBrowser").toUtf8().constData());
	}
	if (model->IsVisible(MENU_ID_PRINT)) {
		model->Remove(MENU_ID_PRINT);
	}
	if (model->IsVisible(MENU_ID_VIEW_SOURCE)) {
		model->Remove(MENU_ID_VIEW_SOURCE);
	}
	model->AddItem(MENU_ITEM_ZOOM_IN, obs_module_text("Zoom.In"));
	if (browser->GetHost()->GetZoomLevel() != 0) {
		model->AddItem(MENU_ITEM_ZOOM_RESET, obs_module_text("Zoom.Reset"));
	}
	model->AddItem(MENU_ITEM_ZOOM_OUT, obs_module_text("Zoom.Out"));
	model->AddSeparator();
	model->InsertItemAt(model->GetCount(), MENU_ITEM_COPY_URL, obs_module_text("CopyUrl"));
	model->InsertItemAt(model->GetCount(), MENU_ITEM_DEVTOOLS, obs_module_text("Inspect"));
	model->InsertCheckItemAt(model->GetCount(), MENU_ITEM_MUTE, QObject::tr("Mute").toUtf8().constData());
	model->SetChecked(MENU_ITEM_MUTE, browser->GetHost()->IsAudioMuted());
}

#if defined(_WIN32)
bool QCefBrowserClient::RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
				       CefRefPtr<CefMenuModel> model, CefRefPtr<CefRunContextMenuCallback> callback)
{
	std::vector<std::tuple<std::string, int, bool, int, bool>> menu_items;
	menu_items.reserve(model->GetCount());
	for (int i = 0; i < model->GetCount(); i++) {
		menu_items.push_back({model->GetLabelAt(i), model->GetCommandIdAt(i), model->IsEnabledAt(i),
				      model->GetTypeAt(i), model->IsCheckedAt(i)});
	}

	QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), [menu_items, callback]() {
		QMenu contextMenu;
		std::string name;
		int command_id;
		bool enabled;
		int type_id;
		bool check;

		for (const std::tuple<std::string, int, bool, int, bool> &menu_item : menu_items) {
			std::tie(name, command_id, enabled, type_id, check) = menu_item;
			switch (type_id) {
			case MENUITEMTYPE_CHECK:
			case MENUITEMTYPE_COMMAND: {
				QAction *item = new QAction(name.c_str());
				item->setEnabled(enabled);
				if (type_id == MENUITEMTYPE_CHECK) {
					item->setCheckable(true);
					item->setChecked(check);
				}
				item->setProperty("cmd_id", command_id);
				contextMenu.addAction(item);
			} break;
			case MENUITEMTYPE_SEPARATOR:
				contextMenu.addSeparator();
				break;
			}
		}

		QAction *action = contextMenu.exec(QCursor::pos());
		if (action) {
			QVariant cmdId = action->property("cmd_id");
			callback.get()->Continue(cmdId.toInt(), EVENTFLAG_NONE);
		} else {
			callback.get()->Cancel();
		}
	});
	return true;
}
#endif

bool QCefBrowserClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
					     CefRefPtr<CefContextMenuParams> params, int command_id,
					     CefContextMenuHandler::EventFlags)
{
	if (command_id < MENU_ID_CUSTOM_FIRST)
		return false;
	CefRefPtr<CefBrowserHost> host = browser->GetHost();
	CefWindowInfo windowInfo;
	QPoint pos;
	switch (command_id) {
	case MENU_ITEM_DEVTOOLS: {
		// DevTools window positioning needs the widget's screen
		// coordinates. Post to UI thread to access safely.
		auto weakSession = std::weak_ptr<BrowserSession>(session);
		int inspectX = params->GetXCoord();
		int inspectY = params->GetYCoord();

		QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), [weakSession, host, inspectX,
										   inspectY]() {
			CefWindowInfo windowInfo;
			QPoint pos(0, 0);

			auto s = weakSession.lock();
			if (s) {
				QCefWidgetInternal *w = s->getWidget();
				if (w)
					pos = w->mapToGlobal(QPoint(0, 0));
			}

#if defined(_WIN32) && CHROME_VERSION_BUILD < 6533
			windowInfo.SetAsPopup(host->GetWindowHandle(), "");
#endif
			windowInfo.bounds.x = pos.x();
			windowInfo.bounds.y = pos.y() + 30;
			windowInfo.bounds.width = 900;
			windowInfo.bounds.height = 700;
			host->ShowDevTools(windowInfo, host->GetClient(), CefBrowserSettings(), {inspectX, inspectY});
		});
		return true;
	}
	case MENU_ITEM_MUTE:
		host->SetAudioMuted(!host->IsAudioMuted());
		return true;
	case MENU_ITEM_ZOOM_IN:
		return zoomPage(host, 1);
	case MENU_ITEM_ZOOM_RESET:
		return zoomPage(host, 0);
	case MENU_ITEM_ZOOM_OUT:
		return zoomPage(host, -1);
	case MENU_ITEM_COPY_URL:
		std::string url = browser->GetMainFrame()->GetURL().ToString();
		auto saveClipboard = [url]() {
			QClipboard *clipboard = QApplication::clipboard();

			clipboard->setText(url.c_str(), QClipboard::Clipboard);

			if (clipboard->supportsSelection()) {
				clipboard->setText(url.c_str(), QClipboard::Selection);
			}
		};
		QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), saveClipboard);
		return true;
		break;
	}
	return false;
}

void QCefBrowserClient::OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType)
{
	if (!frame->IsMain())
		return;

	std::string script = "window.close = () => ";
	script += "console.log(";
	script += "'OBS browser docks cannot be closed using JavaScript.'";
	script += ");";
	frame->ExecuteJavaScript(script, "", 0);
}

void QCefBrowserClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int)
{
	if (!frame->IsMain())
		return;

	std::string s = session->getScript();
	if (!s.empty())
		frame->ExecuteJavaScript(s, CefString(), 0);
}

bool QCefBrowserClient::OnJSDialog(CefRefPtr<CefBrowser>, const CefString &,
				   CefJSDialogHandler::JSDialogType dialog_type, const CefString &message_text,
				   const CefString &default_prompt_text, CefRefPtr<CefJSDialogCallback> callback,
				   bool &)
{
	QString parentTitle;
	QCefWidgetInternal *w = session->getWidget();
	if (w && w->parentWidget())
		parentTitle = w->parentWidget()->windowTitle();
	else
		parentTitle = QStringLiteral("Browser");

	std::string default_value = default_prompt_text;
	QString msg_raw(message_text.ToString().c_str());
	// Replace <br> with standard newline as we will render in plaintext
	msg_raw.replace(QRegularExpression("<br\\s{0,1}\\/{0,1}>"), "\n");

	auto weakSession = std::weak_ptr<BrowserSession>(session);

	QMetaObject::invokeMethod(QCoreApplication::instance()->thread(), [weakSession, msg_raw, default_value,
									   dialog_type, callback]() {
		QString parentTitle;
		auto s = weakSession.lock();
		if (s) {
			QCefWidgetInternal *w = s->getWidget();
			if (w && w->parentWidget())
				parentTitle = w->parentWidget()->windowTitle();
		}
		if (parentTitle.isEmpty())
			parentTitle = QStringLiteral("Browser");

		QString submsg = QString(obs_module_text("Dialog.ReceivedFrom")).arg(parentTitle);
		QString msg = QString("%1\n\n\n%2").arg(msg_raw).arg(submsg);

		if (dialog_type == JSDIALOGTYPE_PROMPT) {

			QInputDialog *dlg = new QInputDialog(nullptr);
			dlg->setWindowFlag(Qt::WindowStaysOnTopHint, true);
			dlg->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
			std::stringstream title;
			title << obs_module_text("Dialog.Prompt") << ": " << obs_module_text("Dialog.BrowserDock");
			dlg->setWindowTitle(title.str().c_str());
			if (!default_value.empty())
				dlg->setTextValue(default_value.c_str());

			auto finished = [callback, dlg](int result) {
				callback.get()->Continue(result == QDialog::Accepted,
							 dlg->textValue().toUtf8().constData());
			};

			QWidget::connect(dlg, &QInputDialog::finished, finished);
			dlg->open();
			if (QLabel *lbl = dlg->findChild<QLabel *>()) {
				// Force plaintext manually
				lbl->setTextFormat(Qt::PlainText);
			}
			dlg->setLabelText(msg);

			return true;
		}

		QMessageBox *dlg = new QMessageBox(nullptr);
		dlg->setStandardButtons(QMessageBox::Ok);
		dlg->setWindowFlag(Qt::WindowStaysOnTopHint, true);
		dlg->setTextFormat(Qt::PlainText);
		dlg->setText(msg);
		std::stringstream title;
		switch (dialog_type) {
		case JSDIALOGTYPE_CONFIRM:
			title << obs_module_text("Dialog.Confirm");
			dlg->setIcon(QMessageBox::Question);
			dlg->addButton(QMessageBox::Cancel);
			break;
		case JSDIALOGTYPE_ALERT:
		default:
			title << obs_module_text("Dialog.Alert");
			dlg->setIcon(QMessageBox::Information);
			break;
		}
		title << ": " << obs_module_text("Dialog.BrowserDock");
		dlg->setWindowTitle(title.str().c_str());

		auto finished = [callback](int result) {
			callback.get()->Continue(result == QMessageBox::Ok, "");
		};

		QWidget::connect(dlg, &QMessageBox::finished, finished);

		dlg->open();
		return true;
	});
	return true;
}

bool QCefBrowserClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent &event, CefEventHandle, bool *)
{
	if (event.type != KEYEVENT_RAWKEYDOWN)
		return false;

	CefRefPtr<CefBrowserHost> host = browser->GetHost();

	if (event.windows_key_code == 'R' &&
#ifdef __APPLE__
	    (event.modifiers & EVENTFLAG_COMMAND_DOWN) != 0) {
#else
	    (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
#endif
		browser->ReloadIgnoreCache();
		return true;
	} else if ((event.windows_key_code == 189 || event.windows_key_code == 109) &&
		   (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
		return zoomPage(host, -1);
	} else if ((event.windows_key_code == 187 || event.windows_key_code == 107) &&
		   (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
		return zoomPage(host, 1);
	} else if ((event.windows_key_code == 48 || event.windows_key_code == 96) &&
		   (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
		return zoomPage(host, 0);
	}
	return false;
}
