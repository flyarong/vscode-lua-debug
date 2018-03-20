#include "dbg_impl.h"
#include "dbg_protocol.h"  	
#include "dbg_io.h" 
#include "dbg_format.h"
#include "dbg_thread.h"
#include "dbg_thunk.h"
#include <thread>
#include <atomic>

namespace vscode
{
	lua_State* get_mainthread(lua_State* thread)
	{
		lua_rawgeti(thread, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
		lua_State* ml = lua_tothread(thread, -1);
		lua_pop(thread, 1);
		return ml;
	}

	static void debugger_hook(debugger_impl* dbg, lua_State *L, lua::Debug *ar)
	{
		dbg->hook(L, ar);
	}

	static void debugger_panic(debugger_impl* dbg, lua_State *L)
	{
		dbg->exception(L);
	}

	void debugger_impl::attach_lua(lua_State* L)
	{
		L = get_mainthread(L);
		if (hookL_.insert(L).second) {
			if (!thunk_hook_) {
				thunk_hook_ = (lua_Hook)thunk_create_hook(
					reinterpret_cast<intptr_t>(this), 
					reinterpret_cast<intptr_t>(&debugger_hook)
				);
			}
			lua_sethook(L, thunk_hook_, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);

			lua_CFunction thunk_panic = (lua_CFunction)thunk_create_panic(
				reinterpret_cast<intptr_t>(this),
				reinterpret_cast<intptr_t>(&debugger_panic), 
				reinterpret_cast<intptr_t>(lua_atpanic(L, 0))
			);
			lua_atpanic(L, thunk_panic);
		}
	}

	void debugger_impl::detach_lua(lua_State* L)
	{
		L = get_mainthread(L);
		if (hookL_.find(L) != hookL_.end()) {
			lua_sethook(L, 0, 0, 0);
		}
	}

	void debugger_impl::detach_all()
	{
		for (auto& L : hookL_)
		{
			lua_sethook(L, 0, 0, 0);
		}
		hookL_.clear();
	}

	bool debugger_impl::update_main(rprotocol& req, bool& quit)
	{
		auto it = main_dispatch_.find(req["command"].Get<std::string>());
		if (it != main_dispatch_.end())
		{
			quit = it->second(req);
			return true;
		}
		return false;
	}

	bool debugger_impl::update_hook(rprotocol& req, lua_State *L, lua::Debug *ar, bool& quit)
	{
		auto it = hook_dispatch_.find(req["command"].Get<std::string>());
		if (it != hook_dispatch_.end())
		{
			quit = it->second(req, L, ar);
			return true;
		}
		return false;
	}

	void debugger_impl::update_redirect()
	{
		if (stdout_) {
			size_t n = stdout_->peek();
			if (n > 0) {
				hybridarray<char, 1024> buf(n);
				stdout_->read(buf.data(), buf.size());
				output("stdout", buf.data(), buf.size(), nullptr);
			}
		}
		if (stderr_) {
			size_t n = stderr_->peek();
			if (n > 0) {
				hybridarray<char, 1024> buf(n);
				stderr_->read(buf.data(), buf.size());
				output("stderr", buf.data(), buf.size(), nullptr);
			}
		}
	}

	static int get_stacklevel(lua_State* L)
	{
		lua::Debug ar;
		int n;
		for (n = 0; lua_getstack(L, n + 1, (lua_Debug*)&ar) != 0; ++n)
		{ }
		return n;
	}

	static int get_stacklevel(lua_State* L, int pos)
	{
		lua::Debug ar;
		if (lua_getstack(L, pos, (lua_Debug*)&ar) != 0) {
			for (; lua_getstack(L, pos + 1, (lua_Debug*)&ar) != 0; ++pos)
			{ }
		}
		else if (pos > 0) {
			for (--pos; pos > 0 && lua_getstack(L, pos, (lua_Debug*)&ar) == 0; --pos)
			{ }
		}
		return pos;
	}

	void debugger_impl::set_step(step step)
	{
		step_ = step;
	}

	bool debugger_impl::is_step(step step)
	{
		return step_ == step;
	}

	bool debugger_impl::check_step(lua_State* L, lua::Debug* ar)
	{
		return stepping_lua_state_ == L && stepping_current_level_ <= stepping_target_level_;
	}

	void debugger_impl::step_in()
	{
		set_state(state::stepping);
		set_step(step::in);
	}

	void debugger_impl::step_over(lua_State* L, lua::Debug* ar)
	{
		set_state(state::stepping);
		set_step(step::over);
		stepping_target_level_ = stepping_current_level_ = get_stacklevel(L);
		stepping_lua_state_ = L;
	}

	void debugger_impl::step_out(lua_State* L, lua::Debug* ar)
	{
		set_state(state::stepping);
		set_step(step::out);
		stepping_target_level_ = stepping_current_level_ = get_stacklevel(L);
		stepping_target_level_--;
		stepping_lua_state_ = L;
	}

	void debugger_impl::hook(lua_State *L, lua::Debug *ar)
	{
		if (!allowhook_) { return; }

		std::lock_guard<dbg_thread> lock(*thread_);

		if (ar->event == LUA_HOOKCALL)
		{
			has_source_ = false;
			if (stepping_lua_state_ == L) {
				stepping_current_level_++;
			}
			return;
		}
		if (ar->event == LUA_HOOKRET)
		{
			has_source_ = false;
			if (stepping_lua_state_ == L) {
				stepping_current_level_ = get_stacklevel(L, stepping_current_level_) - 1;
			}
			return;
		}
		if (ar->event != LUA_HOOKLINE)
			return;
		if (is_state(state::terminated) || is_state(state::birth) || is_state(state::initialized)) {
			return;
		}

		bool bp = check_breakpoint(L, ar);
		if (!bp) {
			if (is_state(state::running)) {
				return thread_->update();
			}
			else if (is_state(state::stepping) && !is_step(step::in) && !check_step(L, ar)) {
				return thread_->update();
			}
			else {
				assert(is_state(state::stepping));
			}
		}

		run_stopped(L, ar, bp? "breakpoint": "step");
	}

	void debugger_impl::exception(lua_State* L)
	{
		std::lock_guard<dbg_thread> lock(*thread_);

		if (!exception_)
		{
			return;
		}

		allowhook_ = false;
		lua::Debug ar;
		if (lua_getstack(L, 0, (lua_Debug*)&ar))
		{
			run_stopped(L, &ar, "exception");
		}
		allowhook_ = true;
	}

	void debugger_impl::run_stopped(lua_State *L, lua::Debug *ar, const char* reason)
	{
		event_stopped(reason);
		step_in();
		if (watch_) watch_->clear(L);

		bool quit = false;
		while (!quit)
		{
			update_redirect();
			network_->update(0);

			rprotocol req = network_->input();
			if (req.IsNull()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			if (req["type"] != "request") {
				continue;
			}
			if (is_state(state::birth)) {
				if (req["command"] == "initialize") {
					request_initialize(req);
					continue;
				}
			}
			else {
				if (update_main(req, quit)) {
					continue;
				}
				if (update_hook(req, L, ar, quit)) {
					continue;
				}
			}
			response_error(req, format("%s not yet implemented", req["command"].GetString()).c_str());
		}
	}

	void debugger_impl::run_idle()
	{
		update_redirect();
		network_->update(0);
		if (is_state(state::birth))
		{
			rprotocol req = network_->input();
			if (req.IsNull()) {
				return;
			}
			if (req["type"] != "request") {
				return;
			}
			if (req["command"] == "initialize") {
				request_initialize(req);
				return;
			}
			response_error(req, format("%s not yet implemented", req["command"].GetString()).c_str());
		}
		else if (is_state(state::initialized) || is_state(state::running) || is_state(state::stepping))
		{
			rprotocol req = network_->input();
			if (req.IsNull()) {
				return;
			}
			if (req["type"] != "request") {
				return;
			}
			bool quit = false;
			if (!update_main(req, quit)) {
				response_error(req, format("%s not yet implemented", req["command"].GetString()).c_str());
				return;
			}
		}
		else if (is_state(state::terminated))
		{
			set_state(state::birth);
		}
	}

	void debugger_impl::update()
	{
		{
			std::unique_lock<dbg_thread> lock(*thread_, std::try_to_lock_t());
			if (!lock) {
				return;
			}
			run_idle();
		}
	}

	void debugger_impl::wait_attach()
	{
		if (thread_->mode() == threadmode::async) {
			semaphore sem;
			attach_callback_ = [&]() {
				sem.signal();
			};
			sem.wait();
			attach_callback_ = std::function<void()>();
		}
	}

	void debugger_impl::set_custom(custom* custom)
	{
		custom_ = custom;
	}

	void debugger_impl::set_coding(coding coding)
	{
		pathconvert_.set_coding(coding);
	}

	void debugger_impl::output(const char* category, const char* buf, size_t len, lua_State* L)
	{
		if (console_ == "none") {
			return;
		}
		wprotocol res;
		for (auto _ : res.Object())
		{
			res("type").String("event");
			res("seq").Int64(seq++);
			res("event").String("output");
			for (auto _ : res("body").Object())
			{
				res("category").String(category);
				if (console_ == "ansi") {
					res("output").String(vscode::a2u(strview(buf, len)));
				}
				else {
					res("output").String(strview(buf, len));
				}

				lua::Debug entry;
				if (L && lua_getstack(L, 1, (lua_Debug*)&entry))
				{
					int status = lua_getinfo(L, "Sln", (lua_Debug*)&entry);
					assert(status);
					const char *src = entry.source;
					if (*entry.what == 'C')
					{
					}
					else if (*entry.source == '@' || *entry.source == '=')
					{
						std::string path;
						if (pathconvert_.get(src, path))
						{
							for (auto _ : res("source").Object())
							{
								res("name").String(w2u(fs::path(u2w(path)).filename().wstring()));
								res("path").String(path);
							};
							res("line").Int(entry.currentline);
							res("column").Int(1);
						}
					}
					else
					{
						intptr_t reference = (intptr_t)src;
						for (auto _ : res("source").Object())
						{
							res("name").String("<Memory>");
							res("sourceReference").Int64(reference);
						}
						res("line").Int(entry.currentline);
						res("column").Int(1);
					}
				}
			}
		}
		network_->output(res);
	}

	void debugger_impl::redirect_stdout()
	{
		stdout_.reset(new redirector);
		stdout_->open("stdout", std_fd::STDOUT);
	}
		
	void debugger_impl::redirect_stderr()
	{
		stderr_.reset(new redirector);
		stderr_->open("stderr", std_fd::STDERR);
	}

	debugger_impl::~debugger_impl()
	{
		thunk_destory(thunk_hook_);
	}

#define DBG_REQUEST_MAIN(name) std::bind(&debugger_impl:: ## name, this, std::placeholders::_1)
#define DBG_REQUEST_HOOK(name) std::bind(&debugger_impl:: ## name, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)

	debugger_impl::debugger_impl(io* io, threadmode mode, coding coding)
		: seq(1)
		, network_(io)
		, state_(state::birth)
		, step_(step::in)
		, stepping_target_level_(0)
		, stepping_current_level_(0)
		, stepping_lua_state_(NULL)
		, breakpoints_()
		, stack_()
		, watch_()
		, pathconvert_(this, coding)
		, custom_(nullptr)
		, thunk_hook_(0)
		, has_source_(false)
		, cur_source_(0)
		, exception_(false)
		, hookL_()
		, attach_callback_()
		, console_("none")
		, allowhook_(true)
		, thread_(mode == threadmode::async 
			? (dbg_thread*)new async(std::bind(&debugger_impl::update, this))
			: (dbg_thread*)new sync(std::bind(&debugger_impl::run_idle, this)))
		, main_dispatch_
		({
			{ "launch", DBG_REQUEST_MAIN(request_attach) },
			{ "attach", DBG_REQUEST_MAIN(request_attach) },
			{ "configurationDone", DBG_REQUEST_MAIN(request_configuration_done) },
			{ "disconnect", DBG_REQUEST_MAIN(request_disconnect) },
			{ "setBreakpoints", DBG_REQUEST_MAIN(request_set_breakpoints) },
			{ "setExceptionBreakpoints", DBG_REQUEST_MAIN(request_set_exception_breakpoints) },
			{ "pause", DBG_REQUEST_MAIN(request_pause) },
		})
		, hook_dispatch_
		({
			{ "continue", DBG_REQUEST_HOOK(request_continue) },
			{ "next", DBG_REQUEST_HOOK(request_next) },
			{ "stepIn", DBG_REQUEST_HOOK(request_stepin) },
			{ "stepOut", DBG_REQUEST_HOOK(request_stepout) },
			{ "stackTrace", DBG_REQUEST_HOOK(request_stack_trace) },
			{ "scopes", DBG_REQUEST_HOOK(request_scopes) },
			{ "variables", DBG_REQUEST_HOOK(request_variables) },
			{ "setVariable", DBG_REQUEST_HOOK(request_set_variable) },
			{ "source", DBG_REQUEST_HOOK(request_source) },
			{ "threads", DBG_REQUEST_HOOK(request_thread) },
			{ "evaluate", DBG_REQUEST_HOOK(request_evaluate) },
			{ "exceptionInfo", DBG_REQUEST_HOOK(request_exception_info) },
		})
	{
		thread_->start();
	}

#undef DBG_REQUEST_MAIN	 
#undef DBG_REQUEST_HOOK
}
