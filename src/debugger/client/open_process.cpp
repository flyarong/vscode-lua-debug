#include <base/hook/injectdll.h>
#include <bee/utility/path_helper.h>
#include <debugger/protocol.h>

bool open_process_with_debugger(vscode::rprotocol& req, int pid)
{
#if defined(_M_X64)
	return false;
#else
	auto dir = bee::path_helper::exe_path().value().parent_path().parent_path();
	if (!base::hook::injectdll(pid, dir / L"x86" / L"debugger-inject.dll", dir / L"x64" / L"debugger-inject.dll")) {
		return false;
	}
	return true;
#endif
}
