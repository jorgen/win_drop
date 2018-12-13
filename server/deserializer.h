#pragma once

#include <memory.h>

struct DeSerializer
{
	DeSerializer(const void *buffer, size_t buffer_size)
		: buffer(reinterpret_cast<const uint8_t *>(buffer))
		, buffer_size(buffer_size)
		, offset(0)
	{
	}

	bool read_to(void *target, size_t size)
	{
	  if (offset + size > buffer_size)
		  return false;
	  memcpy(target, buffer + offset, size);
	  offset += size;
	  return true;
	}

	template<typename T>
		bool read_to_type(T &target)
		{
		return read_to(&target, sizeof(target));
	}

	const uint8_t *buffer;
	size_t buffer_size;
	size_t offset;
};
