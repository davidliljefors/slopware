#include "app_util.h"

#include <stdio.h>

#include "os.h"

bool get_settings_path(char* buf, i32 buf_size,
	const char* app_name, const char* filename)
{
	buf[0] = '\0';

	char appdata[512];
	if (!os_get_appdata_dir(appdata, (i32)sizeof(appdata)))
		return false;

	char dir[512];
	int len = snprintf(dir, sizeof(dir), "%s\\%s", appdata, app_name);
	if (len <= 0 || len >= (int)sizeof(dir))
		return false;

	os_create_directory(dir);

	if (filename)
		len = snprintf(buf, buf_size, "%s\\%s", dir, filename);
	else
		len = snprintf(buf, buf_size, "%s", dir);

	return len > 0 && len < buf_size;
}
