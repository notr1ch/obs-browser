#pragma once

#include "cef-headers.hpp"

#include <memory>
#include <mutex>
#include <functional>

class QCefWidgetInternal;

extern bool QueueCEFTask(std::function<void()> task);
extern void detachBrowserWindow(CefRefPtr<CefBrowserHost> host);
extern void SetBrowserSize(CefRefPtr<CefBrowser> browser, QSize size);

class BrowserSession : public std::enable_shared_from_this<BrowserSession> {
	std::mutex mutex;
	CefRefPtr<CefBrowser> browser;
	QCefWidgetInternal *widget = nullptr;
	QSize widgetSize;
	std::string script;
	enum class State { Idle, Creating, Active } state = State::Idle;

public:
	explicit BrowserSession(QCefWidgetInternal *w) : widget(w) {}

	~BrowserSession()
	{
		// Ensure the browser and widget are properly detached before destruction
		assert(browser == nullptr);
		assert(widget == nullptr);
		assert(state == State::Idle);
	}

	// Called on UI thread before we tell CEF to create a browser.
	void onBeforeCreate()
	{
		std::lock_guard<std::mutex> lock(mutex);

		// We should never have a browser if we're about to create one
		assert(browser == nullptr);
		assert(state == State::Idle);

		state = State::Creating;
	}

	// Called on CEF thread after CreateBrowserSync succeeds.
	void onBrowserCreated(CefRefPtr<CefBrowser> b)
	{
		std::lock_guard<std::mutex> lock(mutex);

		// Every onBrowserCreated should be matched by a close() call, so we should never have a browser already.
		assert(browser == nullptr);

		// If we got set to idle, the Qt widget was hidden or destroyed while we were creating the browser.
		if (state == State::Idle) {
			detachBrowserWindow(b->GetHost());
			b->GetHost()->CloseBrowser(true);
			return;
		}

		state = State::Active;

		// Apply any pending size changes that occurred while the browser was being created.
		if (widgetSize.isValid()) {
			SetBrowserSize(b, widgetSize);
		}

		browser = b;
	}

	// Called on CEF thread if browser creation fails. Reset things to allow for a retry.
	void onBrowserCreationFailed()
	{
		std::lock_guard<std::mutex> lock(mutex);

		assert(state == State::Creating);
		state = State::Idle;
	}

	// Called on UI thread from the widget destructor and hide event. Splits the browser off the session so it can shut
	// down independently, the widget is free to create a new browser after this returns which should not cause any
	// issues.
	void close()
	{
		CefRefPtr<CefBrowserHost> host;

		{
			std::lock_guard<std::mutex> lock(mutex);

			state = State::Idle;

			// Browser either hasn't been created yet (mid-creation will be handled by onBrowserCreated
			// seeing state == State::Idle), or it was already closed.
			if (!browser)
				return;

			host = browser->GetHost();
			browser = nullptr;
		}

		// Detach the native window on the UI thread while the widget's QWindow / container are still alive.
		// This prevents CEF from walking up to OBS's main window and closing it.
		detachBrowserWindow(host);

		// Queue the actual browser close on the CEF thread.
		QueueCEFTask([host]() { host->CloseBrowser(true); });
	}

	// The widget is being destroyed, detach it from the session so the browser can continue to exist.
	void detachWidget()
	{
		std::lock_guard<std::mutex> lock(mutex);
		assert(widget != nullptr);
		widget = nullptr;
	}

	// UI can send size events before the browser is created, store the last known size to use after creation.
	void setSize(QSize size)
	{
		std::lock_guard<std::mutex> lock(mutex);
		widgetSize = size;
	}

	// UI can set a script to be executed in the browser, accessed by both the UI and CEF threads.
	void setScript(const std::string &s)
	{
		std::lock_guard<std::mutex> lock(mutex);
		script = s;
	}

	std::string getScript()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return script;
	}

	// Called on UI thread. Returns true if the browser is creating or active.
	bool isActive()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return state != State::Idle;
	}

	// Called on UI thread. Returns the browser if still valid.
	CefRefPtr<CefBrowser> getBrowser()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return browser;
	}

	// Called on UI thread. Returns the widget if still attached.
	QCefWidgetInternal *getWidget()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return widget;
	}
};
