#include "win_global.h"

#include <stdlib.h>
#include <stdio.h>

#include "server.h"

int __cdecl main(int argc, char **argv) 
{
	std::string path;
	if (argc == 2)
	{
		HANDLE dir_handle = CreateFile(
			s2ws(argv[1]).c_str(),
			FILE_LIST_DIRECTORY,
			FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS,
			NULL);
		if (dir_handle == INVALID_HANDLE_VALUE)
		{
			fprintf(stderr, "Failed to open directory %s\n", argv[1]);
			return -1;
		}
		std::wstring working_dir;
		working_dir.resize(4096);
		GetFinalPathNameByHandle(dir_handle, &working_dir[0], DWORD(working_dir.size()), NULL);
		path = sw2s(working_dir);
		CloseHandle(dir_handle);
		SetCurrentDirectoryW(working_dir.c_str());
	}
	else if (argc == 1)
	{
		std::wstring p;
		p.resize(4096);
		GetCurrentDirectoryW(4096, &p[0]);
		path = std::string("\\\\?\\") + sw2s(p);
	}
	else
	{
		printf("usage: pexip_dropbox [directory]\n");
		return 1;
	}

	WSADATA wsa_data;
	int failed = WSAStartup(MAKEWORD(2,2), &wsa_data);
	if (failed) {
		fprintf(stderr, "WSAStartup failed with error: %d\n", failed);
		return -1;
	}

	if (!run_server(path))
	{
		return -1;
	}
	return 0;
}
