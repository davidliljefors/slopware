#include <windows.h>

#include "example_app.h"
#include "gotofile.h"
#include "gototext.h"
#include "host.h"
#include "taskman.h"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	return host_run(taskman_get_app());
	//return host_run(gotofile_get_app());
	//return host_run(gototext_get_app());
	//return host_run(example_app_get_app());
}

