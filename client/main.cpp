#include "win_global.h"
#include <stdlib.h>
#include <stdio.h>

#include "client.h"

int __cdecl main(int argc, char **argv) 
{
	WSADATA wsaData;
	int iResult;
	iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		return 1;
	}

	if (argc < 2 || argc > 3) {
		printf("usage: pexip_dropbox [directory] server-name\n");
		return 1;
	}

	std::string dir_name;
	std::string server_name;
	if (argc == 2)
	{
		std::wstring buffer;
		buffer.resize(1024);
		GetCurrentDirectoryW(DWORD(buffer.size()), &buffer[0]);
		dir_name = sw2s(buffer);
		server_name = argv[1];
	}
	else
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
		std::wstring real_sub_w;
		real_sub_w.resize(4096);
		GetFinalPathNameByHandle(dir_handle, &real_sub_w[0], DWORD(real_sub_w.size()), NULL);
		dir_name = sw2s(real_sub_w);
		CloseHandle(dir_handle);
		server_name = argv[2];
	}
	if (!run_client(server_name, dir_name))
	{
		return -1;
	}

    return 0;
}
