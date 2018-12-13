#pragma once

struct Serializer
{
	Serializer(uint8_t *buffer, size_t buffer_size)
		: buffer(buffer)
		, buffer_size(buffer_size)
		, offset(0)
	{}
	uint8_t *buffer;
	size_t buffer_size;
	size_t offset;

	bool add_data(const void *in_data, size_t size)
	{
	  if (offset + size > buffer_size)
		  return false;
	  memcpy(buffer + offset, in_data, size);
	  offset += size;
	  return true;
	}

	template<typename T>
		bool add_typed_data(const T &in_data)
		{
		return add_data(&in_data, sizeof(T));
	}
};
