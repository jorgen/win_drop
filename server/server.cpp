#include <stdio.h>
#include "win_global.h"

#include <string>
#include <algorithm>

#include "deserializer.h"

#include <Shlwapi.h>

#include <assert.h>
#define DEFAULT_PORT 41218

enum class SocketState
{
	NoError,
	Closed,
	Error
};

enum class FileAction
{
	Added = 1,
	Removed = 2,
	Modified = 3,
	Renamed = 4,
};

struct Header
{
	uint64_t full_size;
	uint32_t header_size;
	FileAction action;
	uint8_t sha[20];
	uint32_t path_size;
};

struct FileCloser
{
	FileCloser(HANDLE handle)
		: handle(handle)
	{}
	~FileCloser()
	{
		if (!CloseHandle(handle))
			fprintf(stderr, "Failed to close file: %s\n", error_to_string(GetLastError()).c_str());
	}

	HANDLE handle;
};
static SocketState read_from_socket(SOCKET socket, void *buffer, size_t buffer_size)
{
	char *b = reinterpret_cast<char *>(buffer);
	uint32_t offset = 0;
	while (offset < buffer_size)
	{
		int bytes_received = recv(socket, b + offset, int(buffer_size - offset), 0);
		if (bytes_received == 0)
			return SocketState::Closed;
		if (bytes_received < 0)
		{
		fprintf(stderr, "read_header error: %d\n", WSAGetLastError());
		closesocket(socket);
			return SocketState::Error;
		}
		offset += bytes_received;
	}
	return SocketState::NoError;
}

static SocketState read_header(SOCKET socket, Header &target_header)
{
	char buffer[44];
	SocketState socket_state = read_from_socket(socket, buffer, sizeof(buffer));
	if (socket_state != SocketState::NoError)
		return socket_state;

	uint32_t offset = 0;
	if (memcmp(buffer, "PID0", 4))
	{
		fprintf(stderr, "Wrong magic in header: %d\n", WSAGetLastError());
		closesocket(socket);
		return SocketState::Error;
	}
	DeSerializer ds(buffer + 4, sizeof(buffer));
	ds.read_to_type(target_header.full_size);
	ds.read_to_type(target_header.header_size);
	ds.read_to_type(target_header.action);
	ds.read_to(target_header.sha, 20);
	if (!ds.read_to_type(target_header.path_size)
		|| target_header.full_size < target_header.header_size
		|| target_header.action < FileAction::Added
		|| target_header.action > FileAction::Renamed)
	{
		fprintf(stderr, "Wrong header content: %d\n", WSAGetLastError());
		closesocket(socket);
		return SocketState::Error;
	}
	return SocketState::NoError;
}

static bool file_exist(DWORD attr)
{
	return attr != INVALID_FILE_ATTRIBUTES;
}

static bool is_sub_path(const std::string &parent, const std::string &sub_path)
{
	std::wstring sub_path_w = s2ws(sub_path);
	DWORD attr = GetFileAttributesW(sub_path_w.c_str());
	bool create = !file_exist(attr);
	HANDLE file_handle = CreateFileW(sub_path_w.c_str(),
		GENERIC_WRITE | GENERIC_READ,
		NULL,
		NULL,
		create ? OPEN_ALWAYS : OPEN_EXISTING,
		NULL,
		NULL);
	if (file_handle == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Failed to open inputfile for verification. Skipping file %s\n", sub_path.c_str());
		return false;
	}

	std::wstring real_sub_w;
	real_sub_w.resize(4096);
	GetFinalPathNameByHandle( file_handle, &real_sub_w[0], DWORD(real_sub_w.size()), NULL);
	std::string real_sub = sw2s(real_sub_w);
	CloseHandle(file_handle);
	if (create)
		DeleteFileW(real_sub_w.c_str());

	return real_sub.find(parent) == 0;
}

static SocketState handle_added_modified(const std::string &target_directory, SOCKET socket, Header &header)
{
	fprintf(stderr, "Add/Modify\n");
	char buffer[1 << 15];
	uint32_t read_size = uint32_t(std::min(header.path_size + (header.full_size - header.header_size), sizeof(buffer)));
	SocketState socket_state = read_from_socket(socket, buffer, read_size);
	if (socket_state != SocketState::NoError)
		return socket_state;
	std::string path(buffer, header.path_size);
	if (!is_sub_path(target_directory, path))
	{
		fprintf(stderr, "illigal path specified. Not a sub path of %s -> %s\n", target_directory.c_str(), path.c_str());
		closesocket(socket);
		return SocketState::Error;
	}
	HANDLE file_handle = CreateFileW(s2ws(path).c_str(),
		GENERIC_WRITE,
		NULL,
		NULL,
		CREATE_ALWAYS,
		NULL,
		NULL);
	if (file_handle == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Failed to open file for creation/modification %s\n", path.c_str());
		closesocket(socket);
		return SocketState::Error;
	}
	FileCloser closer(file_handle);

	uint64_t full_bytes_written = 0;
	DWORD bytes_written;
	if (!WriteFile(file_handle, buffer + header.path_size, read_size - header.path_size, &bytes_written, NULL))
	{
		fprintf(stderr, "Failed to write to file %s: %s\n", path.c_str(), error_to_string(GetLastError()).c_str());
		closesocket(socket);
		return SocketState::Error;
	}
	assert(bytes_written == read_size - header.path_size);
	full_bytes_written += bytes_written;

	uint64_t file_size = header.full_size - header.header_size;
	while (full_bytes_written < file_size)
	{
		read_size = uint32_t(std::min(file_size - full_bytes_written, sizeof(buffer)));
		SocketState socket_state = read_from_socket(socket, buffer, read_size);
		if (socket_state != SocketState::NoError)
			return socket_state;

		if (!WriteFile(file_handle, buffer, read_size, &bytes_written, NULL))
		{
			fprintf(stderr, "Failed to write to file %s: %s\n", path.c_str(), error_to_string(GetLastError()).c_str());
			closesocket(socket);
			return SocketState::Error;
		}
		assert(bytes_written == read_size);
		full_bytes_written += bytes_written;
	}

	return SocketState::NoError;
}

static SocketState handle_remove(const std::string &target_directory, SOCKET socket, Header &header)
{
	fprintf(stderr, "Remove\n");
	char buffer[1 << 12];
	if (header.full_size - header.header_size)
	{
		fprintf(stderr, "illigal datasize for removing file\n");
		closesocket(socket);
		return SocketState::Error;
	}
	uint32_t read_size = uint32_t(std::min(header.path_size + (header.full_size - header.header_size), sizeof(buffer)));
	SocketState socket_state = read_from_socket(socket, buffer, read_size);
	if (socket_state != SocketState::NoError)
		return socket_state;
	std::string path(buffer, header.path_size);
	if (!is_sub_path(target_directory, path))
	{
		fprintf(stderr, "illigal path specified. Not a sub path of %s -> %s\n", target_directory.c_str(), path.c_str());
		closesocket(socket);
		return SocketState::Error;
	}
	DeleteFileW(s2ws(path).c_str());
	return SocketState::NoError;
}

static SocketState handle_rename(const std::string &target_directory, SOCKET socket, Header &header)
{
	fprintf(stderr, "Rename\n");
	char buffer[1 << 13];
	if (sizeof(buffer) < header.full_size - header.header_size + header.path_size)
	{
		fprintf(stderr, "illigal datasize for renaming. Giving up\n");
		closesocket(socket);
		return SocketState::Error;
	}
	uint32_t read_size = uint32_t(std::min(header.path_size + (header.full_size - header.header_size), sizeof(buffer)));
	SocketState socket_state = read_from_socket(socket, buffer, read_size);
	if (socket_state != SocketState::NoError)
		return socket_state;
	std::string path(buffer, header.path_size);
	if (!is_sub_path(target_directory, path))
	{
		fprintf(stderr, "illigal path specified. Not a sub path of %s -> %s\n", target_directory.c_str(), path.c_str());
		closesocket(socket);
		return SocketState::Error;
	}

	std::string to_path(buffer + header.path_size, header.full_size - header.header_size);
	if (!is_sub_path(target_directory, to_path))
	{
		fprintf(stderr, "illigal path specified. Not a sub path of %s -> %s\n", target_directory.c_str(), to_path.c_str());
		closesocket(socket);
		return SocketState::Error;
	}
	if (!MoveFileW(s2ws(path).c_str(), s2ws(to_path).c_str()))
	{
		DWORD error = GetLastError();
		fprintf(stderr, "Failed to move filr %s to %s: %d %s\n", path.c_str(), to_path.c_str(), error, error_to_string(error).c_str());
	}
	return SocketState::NoError;
}

static SocketState handle_connection(const std::string &target_directory, SOCKET socket)
{
	while (true)
	{
		Header header;
		SocketState socket_state = read_header(socket, header);
		if (socket_state != SocketState::NoError)
			return socket_state;

		switch (header.action)
		{
		case FileAction::Added:
		case FileAction::Modified:
			socket_state = handle_added_modified(target_directory, socket, header);
			break;
		case FileAction::Removed:
			socket_state = handle_remove(target_directory, socket, header);
			break;
		case FileAction::Renamed:
			socket_state = handle_rename(target_directory, socket, header);
		}

		if (socket_state != SocketState::NoError)
			return socket_state;
	}

}

bool run_server(const std::string &target_directory)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	SOCKET _listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (_listen == INVALID_SOCKET) {
		fprintf(stderr, "socket failed with error: %ld\n", WSAGetLastError());
		WSACleanup();
		return false;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // differs from sender
	addr.sin_port = htons(DEFAULT_PORT);
	int success = bind(_listen, (struct sockaddr*)&addr, sizeof(addr));
	if (success == SOCKET_ERROR) {
		fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
		closesocket(_listen);
		WSACleanup();
		return false;
	}

	success = listen(_listen, SOMAXCONN);
	if (success == SOCKET_ERROR) {
		fprintf(stderr, "listen failed with error: %d\n", WSAGetLastError());
		closesocket(_listen);
		WSACleanup();
		return false;
	}

	SOCKET client;
	while (true)
	{
		client = accept(_listen, NULL, NULL);
		if (client == INVALID_SOCKET) {
			fprintf(stderr, "accept failed with error: %d\n", WSAGetLastError());
			closesocket(_listen);
			WSACleanup();
			return false;
		}

		SOCKADDR_IN info;
		int info_size = sizeof(info);
		memset(&info, 0, sizeof(info));
		getpeername(client, (sockaddr *) &info, &info_size);

		char *ip = inet_ntoa(info.sin_addr);
		fprintf(stderr, "Connection received from ip %s\n", ip);

		handle_connection(target_directory, client);
	}


	closesocket(_listen);

	success = shutdown(client, SD_SEND);
	if (success == SOCKET_ERROR) {
		fprintf(stderr, "shutdown failed with error: %d\n", WSAGetLastError());
		closesocket(client);
		WSACleanup();
		return false;
	}

	closesocket(client);
	WSACleanup();

	return true;
}
