#include "client.h"

#include "win_global.h"

#include <vector>
#include <chrono>

#include <algorithm>

extern "C" {
#include "sha1.h"
}

#include "serializer.h"

#define DEFAULT_PORT "41218"

struct HashedFile
{
	uint64_t frame_sent;
	std::string path;
	char sha1[21];
};

struct CommunicationState
{
	SOCKET socket;
	std::vector<HashedFile> files;
	uint64_t frame = 0;
};

enum class FileAction
{
	Added = 1,
	Removed = 2,
	Modified = 3,
	RenamedOldName = 4,
	RenamedNewName = 5
};

struct FileChange
{
	FileAction action;
	std::string name;
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

static bool read_file(std::string file, std::vector<uint8_t> &data)
{
	HANDLE file_handle = CreateFileW(s2ws(file).c_str(),
		GENERIC_READ,
		NULL,
		NULL,
		OPEN_EXISTING,
		NULL,
		NULL);
	if (file_handle == INVALID_HANDLE_VALUE)
	{
		//fprintf(stderr, "Failed to open inputfile %s : %s\n", file.c_str(), error_to_string(GetLastError()).c_str());
		return false;
	}
	FileCloser closer(file_handle);
	LARGE_INTEGER f_size;
	if (!GetFileSizeEx(file_handle, &f_size))
	{
		fprintf(stderr, "Failed to read file %s size: %s\n", file.c_str(), error_to_string(GetLastError()).c_str());
		return false;
	}

	int64_t actual_size = f_size.QuadPart;
	int64_t size = actual_size + 4096 - (actual_size % 4096);
	data.resize(size_t(size));

	int64_t large_file_read = 0;
	while (large_file_read < actual_size)
	{
		DWORD bytes_read;
		int64_t max_read_size = std::min(int64_t(1) << int64_t(30), size - large_file_read);
		if (!ReadFile(file_handle, data.data() + large_file_read, DWORD(max_read_size), &bytes_read, NULL))
		{
			fprintf(stderr, "Failed to read file: %s %s.\n", file.c_str(), error_to_string(GetLastError()).c_str());
			return false;
		}
		large_file_read += bytes_read;
	}
	data.resize(large_file_read);
	return true;
}

static bool add_dir_handle_to_ol(const std::string &directory, HANDLE dir_handle, std::vector<uint8_t> &notify_buf, OVERLAPPED &ol)
{
	if (!ReadDirectoryChangesW(dir_handle, notify_buf.data(), DWORD(notify_buf.size()), TRUE, FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, NULL, &ol, NULL))
	{
		DWORD error = GetLastError();
		if (error != ERROR_IO_PENDING)
		{
			fprintf(stderr, "Listening for changes to directory %s failed: %s\n", directory.c_str(), error_to_string(error).c_str());
			return false;
		}
	}
	return true;
}

static bool path_is_dir(DWORD attr)
{
	return attr & FILE_ATTRIBUTE_DIRECTORY;
}

static bool file_exist(DWORD attr)
{
	return attr != INVALID_FILE_ATTRIBUTES;
}

static HashedFile *get_hased_file(CommunicationState &state, const std::string &name)
{
	auto it = std::find_if(state.files.begin(), state.files.end(), [&name](const HashedFile &a) { return a.path == name; });
	if (it == state.files.end())
		return nullptr;
	return &(*it);
}

static void drop_hashed_file(CommunicationState &state, const std::string &name)
{
	auto it = std::find_if(state.files.begin(), state.files.end(), [&name](const HashedFile &a) { return a.path == name; });
	if (it != state.files.end())
		state.files.erase(it);
}

static bool send_data(CommunicationState &state, const void *data, int size)
{
	int total_bytes_sent = 0;
	
	while (total_bytes_sent < size)
	{
		int bytes_sent = send(state.socket, (const char *)data + total_bytes_sent, size - total_bytes_sent, 0);
		if (bytes_sent == SOCKET_ERROR)
		{
			fprintf(stderr, "Failed to send %d\n", WSAGetLastError());
			return false;
		}
		total_bytes_sent += bytes_sent;
	}
	return true;
}


static bool send_action(CommunicationState &state, HashedFile *file, FileAction action, const void *data, size_t data_size)
{
	uint8_t header_buffer[4096];
	Serializer s(header_buffer, sizeof(header_buffer));

	char magic[] = { 'P', 'I', 'D', '0' };
	uint32_t header_size = uint32_t(4 + 8 + 4 + 4 + 20 + 4 + file->path.size());
	uint64_t message_size = header_size + data_size;
	s.add_data(magic, 4);
	s.add_typed_data(message_size);
	s.add_typed_data(header_size);
	s.add_typed_data(action);
	s.add_data(file->sha1, 20);
	s.add_typed_data(uint32_t(file->path.size()));
	if (!s.add_data(file->path.data(), file->path.size()))
		return false;

	if (!send_data(state, s.buffer, int(s.offset)))
		return false;

	if (!send_data(state, data, int(data_size)))
		return false;
	return true;
}

static bool process_changed_paths(const std::string parent_dir, std::vector<FileChange> &changes, CommunicationState &state)
{
	state.frame++;
	for (int i = 0; i < changes.size(); i++)
	{
		auto &change = changes[i];
		DWORD attr = GetFileAttributesW(s2ws(parent_dir + change.name).c_str());
		if (file_exist(attr) && path_is_dir(attr))
			continue;
		if (change.action == FileAction::Added
			|| change.action == FileAction::Modified)
		{
			if (!file_exist(attr))	
				continue;
			auto hashed_file = get_hased_file(state, change.name);
			if (hashed_file && hashed_file->frame_sent == state.frame)
				continue;
			std::vector<uint8_t> file_data;
			if (!read_file(parent_dir + change.name, file_data))
				continue;
			if (!hashed_file)
			{
				state.files.push_back({});
				hashed_file = &state.files.back();
				memset(hashed_file->sha1, 0, sizeof(hashed_file->sha1));
				hashed_file->path = change.name;
			}
			hashed_file->frame_sent = state.frame;
			char old_hash[21];
			memcpy(old_hash, hashed_file->sha1, sizeof(old_hash));
			SHA1(hashed_file->sha1, reinterpret_cast<const char *>(file_data.data()), int(file_data.size()));
			if (memcmp(hashed_file->sha1, old_hash, sizeof(old_hash)))
			{
				fprintf(stderr, "New hash on file. Sending %s\n", hashed_file->path.c_str());
				if (!send_action(state, hashed_file, change.action, file_data.data(), file_data.size()))
					return false;
			}
		}
		else if (change.action == FileAction::Removed)
		{
			if (file_exist(attr))
				continue;
			HashedFile *hashed_file = get_hased_file(state, change.name);
			if (!hashed_file)
				continue;
			fprintf(stderr, "Found deletion of hashed file: %s\n", change.name.c_str());
			if (!send_action(state, hashed_file, change.action, nullptr, 0))
				return false;
			drop_hashed_file(state, change.name);
		}
		else if (change.action == FileAction::RenamedOldName)
		{
			HashedFile *hashed_file = get_hased_file(state, change.name);
			if (!hashed_file)
			{
				i++;
				continue;
			}

			fprintf(stderr, "Moved from %s to %s\n", change.name.c_str(), changes[i + 1].name.c_str());
			const std::string &new_name = changes[i + 1].name;
			if (!send_action(state, hashed_file, change.action, new_name.data(), new_name.size()))
				return false;
			hashed_file->path = new_name;
			i++;
		}
	}

	return true;
}

static bool watch_directory(const std::string &directory, CommunicationState &state)
{
	const int seconds_fs_timeout = 1;
	HANDLE dir_handle = CreateFile(
		s2ws(directory).c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,
		NULL);

	std::string dir_slash = directory + "\\";
	std::vector<uint8_t> notify_info;
	notify_info.resize(4096);

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ol.hEvent)
	{
		fprintf(stderr, "Failed to create OVERLAPPED event %s\n", error_to_string(GetLastError()).c_str());
		return false;
	}

	if (!add_dir_handle_to_ol(directory, dir_handle, notify_info, ol))
		return false;

	std::vector<FileChange> files_changed;
	auto time_at_empty = std::chrono::system_clock::now();
	while (true)
	{
		DWORD wait_for;
		if (files_changed.empty())
		{
			wait_for = INFINITE;
		}
		else
		{
			auto elapsed = std::chrono::system_clock::now() - time_at_empty;
			if (elapsed >= std::chrono::seconds(seconds_fs_timeout))
			{
				if (!process_changed_paths(dir_slash, files_changed, state))
					return false;
				files_changed.clear();
				wait_for = INFINITE;
			}
			else
			{
				wait_for = DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(seconds_fs_timeout) - elapsed).count());
			}
		}
		DWORD result = WaitForSingleObject(ol.hEvent, wait_for);
		if (result == WAIT_TIMEOUT)
		{
			if (!process_changed_paths(dir_slash, files_changed, state))
				return false;
			files_changed.clear();
		}
		else if (result == WAIT_OBJECT_0)
		{
			DWORD bytes_read = 0;
			if (!GetOverlappedResult(dir_handle, &ol, &bytes_read, false))
			{
				DWORD error = GetLastError();
				if (error != ERROR_IO_PENDING)
				{
					fprintf(stderr, "Failed to retrieve file system events in directory %s: %s\n", directory.c_str(), error_to_string(error).c_str());
					return false;
				}
			}
			uint32_t offset = 0;
			while (offset < bytes_read)
			{
				if (files_changed.empty())
					time_at_empty = std::chrono::system_clock::now();
				FILE_NOTIFY_INFORMATION *current = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(notify_info.data() + offset);
				FileAction action = FileAction(current->Action);
				std::wstring file_name(current->FileName, current->FileNameLength / sizeof(wchar_t));
				if (action == FileAction::RenamedOldName)
				{
					FILE_NOTIFY_INFORMATION *next = nullptr;
					if (current->NextEntryOffset)
						next = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(notify_info.data() + offset + current->NextEntryOffset);
					if (next && FileAction(next->Action) == FileAction::RenamedNewName)
					{
						std::wstring next_file_name(next->FileName, next->FileNameLength / sizeof(wchar_t));
						files_changed.push_back({ FileAction(current->Action), sw2s(file_name) });
						files_changed.push_back({ FileAction(next->Action), sw2s(next_file_name) });
						offset += current->NextEntryOffset;
						current = next;
					}
					else
					{
						files_changed.push_back({ FileAction::Removed, sw2s(file_name) });
					}
				}
				else if (action == FileAction::RenamedNewName)
				{
					files_changed.push_back({ FileAction::Added, sw2s(file_name) });
				}
				else
				{
					files_changed.push_back({ action, sw2s(file_name) });
				}
				if (current->NextEntryOffset)
					offset += current->NextEntryOffset;
				else
					break;
			}
		}

		ResetEvent(ol.hEvent);
		if (!add_dir_handle_to_ol(directory, dir_handle, notify_info, ol))
			return false;
	}
	return true;
}

bool run_client(const std::string &server_string, const std::string &watch_directory_name)
{
	CommunicationState state;
	struct addrinfo *result = NULL,
		*ptr = NULL,
		hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	int failed = getaddrinfo(server_string.c_str(), DEFAULT_PORT, &hints, &result);
	if (failed)
	{
		fprintf(stderr, "getaddrinfo failed with error: %d\n", failed);
		WSACleanup();
		return false;
	}

	for (ptr = result; ptr != NULL; ptr = ptr->ai_next)
	{

		state.socket = socket(ptr->ai_family, ptr->ai_socktype,
			ptr->ai_protocol);
		if (state.socket == INVALID_SOCKET)
		{
			fprintf(stderr, "socket failed with error: %ld\n", WSAGetLastError());
			WSACleanup();
			return false;
		}

		int success = connect(state.socket, ptr->ai_addr, (int)ptr->ai_addrlen);
		if (success == SOCKET_ERROR)
		{
			closesocket(state.socket);
			state.socket = INVALID_SOCKET;
			continue;
		}
		break;
	}

	freeaddrinfo(result);

	if (state.socket == INVALID_SOCKET)
	{
		fprintf(stderr, "Unable to connect to server!\n");
		WSACleanup();
		return false;
	}

	fprintf(stderr, "Connected. Watching directory %s\n", watch_directory_name.c_str());
	if (!watch_directory(watch_directory_name, state))
	{
		fprintf(stderr, "shutdown failed with error: %d\n", WSAGetLastError());
		closesocket(state.socket);
		WSACleanup();
		return false;
	}

	int success = shutdown(state.socket, SD_SEND);
	if (success == SOCKET_ERROR)
	{
		fprintf(stderr, "shutdown failed with error: %d\n", WSAGetLastError());
		closesocket(state.socket);
		WSACleanup();
		return false;
	}

	closesocket(state.socket);
	WSACleanup();

	return true;
}
