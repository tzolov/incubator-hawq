#ifndef THRIFT_UTIL_H
#define THRIFT_UTIL_H

#define SIGNED_RIGHT_SHIFT_IS 1
#define ARITHMETIC_RIGHT_SHIFT 1

#include <iostream>
#include <stdint.h>
#include <boost/shared_ptr.hpp>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/protocol/TCompactProtocol.h>
#include <sstream>
#include <vector>
#include <thrift/TApplicationException.h>
#include <thrift/protocol/TDebugProtocol.h>
#include <thrift/transport/TBufferTransports.h>

using namespace boost;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;

namespace hawq {
/**
 * Utility class to serialize thrift objects to a binary format.
 * This object should be reused if possible to reuse the underlying memory.
 * Note: thrift will encode NULLs into the serialized buffer so it is not valid
 * to treat it as a string.
 */
class ThriftSerializer {
public:

	ThriftSerializer() {
	}
	;

	ThriftSerializer(bool compact, int initial_buffer_size = 1024) :
			mem_buffer_(new TMemoryBuffer(initial_buffer_size)) {
		if (compact) {
			TCompactProtocolFactoryT<TMemoryBuffer> factory;
			protocol_ = factory.getProtocol(mem_buffer_);
		} else {
			TBinaryProtocolFactoryT<TMemoryBuffer> factory;
			protocol_ = factory.getProtocol(mem_buffer_);
		}
	}

	~ThriftSerializer(){
	}

	/**
	 * Serialize obj into a memory buffer.  The result is returned in buffer/len.
	 * The memory returned is owned by this object and will be invalid
	 * when another object is serialized.
	 * @param obj:		objects to be serialized
	 * @param len:		the length of buffer after serialization
	 * @param buffer:	the buffer which stored the serialization result
	 * @return			return serialize result
	 */
	template<class T>
	int Serialize(T* obj, uint32_t* len, uint8_t** buffer) {
		try {
			mem_buffer_->resetBuffer();
			obj->write(protocol_.get());
		} catch (std::exception& e) {
			std::stringstream msg;
			msg << "Couldn't serialize thrift object:\n" << e.what();
			return -1;
		}
		mem_buffer_->getBuffer(buffer, len);
		return 0;
	}

private:
	shared_ptr<TMemoryBuffer> mem_buffer_;
	shared_ptr<TProtocol> protocol_;
};

class ThriftDeserializer {
public:
	ThriftDeserializer(bool compact) {
	}
	;
	ThriftDeserializer() {
	}
	;

	/**
	 * create a protocol according to the memory transport.
	 */
	shared_ptr<TProtocol> CreateDeserializeProtocol(
			shared_ptr<TMemoryBuffer> mem, bool compact) {
		if (compact) {
			TCompactProtocolFactoryT<TMemoryBuffer> tproto_factory;
			return tproto_factory.getProtocol(mem);
		} else {
			TBinaryProtocolFactoryT<TMemoryBuffer> tproto_factory;
			return tproto_factory.getProtocol(mem);
		}
	}

	/**
	 * Deserialize a thrift message from buf/len. Deserialize msg bytes into c++ thrift msg
	 * using memory transport.
	 * @param buf:		buffer for deserialization
	 * @param len:		buffer length for deserializtion.
	 * 					buf/len must at least contain all the bytes needed to store the thrift message.
	 * 					At return, len will be set to the actual length of the header.
	 * @return			return deserialization result
	 */
	template<class T>
	int DeserializeThriftMsg(const uint8_t* buf,
			uint32_t* len, bool compact, T* deserialized_msg) {

		/* TMemoryBuffer is not const-safe, although we use it in a const-safe way,
		 * so we have to explicitly cast away the const.*/
		shared_ptr<TMemoryBuffer> tmem_transport(
				new TMemoryBuffer(const_cast<uint8_t*>(buf), *len));
		shared_ptr<TProtocol> tproto = CreateDeserializeProtocol(tmem_transport,
				compact);
		try {
			deserialized_msg->read(tproto.get());
		} catch (std::exception& e) {
			std::stringstream msg;
			std::cout << "couldn't deserialize thrift msg:\n" << e.what();
			return -1;
		} catch (...) {
			std::cout<<"Unknown exception"<<std::endl;
			return -1;
		}
		uint32_t bytes_left = tmem_transport->available_read();
		*len = *len - bytes_left;
		return 0;
	}

private:
	shared_ptr<TProtocolFactory> factory_;
	shared_ptr<TProtocol> tproto_;
}
;

}

#endif
