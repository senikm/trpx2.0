
//
// Written by: Senik Matinyan, 2024
//


#include "hdf5.h"
#include "H5PLextern.h"
#include <iostream>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <cstring>
#include "Concurrent.hpp"
#include "Terse_hdf5.hpp"


#define TERSE_FILTER_ID 32029
#define TERSE_DEFAULT_CHUNK_SIZE 1 << 18
#define TYPE_CODE_INT16 0
#define TYPE_CODE_UINT16 1
#define TYPE_CODE_INT32 2
#define TYPE_CODE_UINT32 3
#define TYPE_CODE_INT8 4
#define TYPE_CODE_UINT8 5

/**
 * @brief Packs an HDF5 buffer into a terse-compressed buffer.
 *
 * This function takes an input buffer of type `T` and compresses its contents
 * into a terse-compressed format. If chunking is enabled, the buffer is divided
 * into smaller chunks, which are compressed concurrently.
 *
 * @tparam T The data type of the elements in the buffer.
 * @param data A double pointer to the input/output buffer. The original buffer
 *             will be freed, and a new compressed buffer will be allocated.
 * @param size A pointer to the size of the input/output buffer. This value will
 *             be updated with the size of the compressed buffer.
 * @param chunk_size The size of chunks for concurrent compression. If set to
 *                   zero, the buffer will not be chunked.
 * @return The size of the compressed buffer in bytes, or 0 if an error occurs.
 */
template<typename T>
std::size_t hdf5_buffer_to_terse(void** data, std::size_t* size, std::size_t chunk_size) {
    struct Fixed_ostreambuf : public std::streambuf {
        Fixed_ostreambuf(char* buffer, std::size_t size) { setp(buffer, buffer + size); }
    };
    std::span buffer(static_cast<T*>(*data), *size / sizeof(T));
    jpa::Terse<jpa::Concurrent> terse_chunks;
    jpa::Terse<jpa::Concurrent> terse_rest;
    if (chunk_size == 0)
        terse_chunks.push_back(buffer);
    else {
        std::size_t pos = 0;
        for (; pos + chunk_size < buffer.size(); pos += chunk_size)
            terse_chunks.push_back(std::span(buffer.data() + pos, chunk_size));
        terse_rest.push_back(std::span(buffer.data() + pos, buffer.size() - pos));
    }
    std::size_t terse_size = terse_chunks.file_size() + terse_rest.file_size();
    void* new_buf = malloc(terse_size);
    if (!new_buf) {
        std::cerr << "Allocation of memory failed while compressing data in hdf5_buffer_to_terse"<< std::endl;
        return 0;
    }
    Fixed_ostreambuf oss_buffer(static_cast<char*>(new_buf), terse_size);
    std::ostream oss(&oss_buffer);
    terse_chunks.write(oss);
    terse_rest.write(oss);
    free(buffer.data());
    *data = new_buf;
    return *size = terse_size;
}

/**
 * @brief Unpacks a terse-compressed buffer back into an HDF5 buffer.
 *
 * This function reads a terse-compressed buffer and reconstructs the original
 * data into a buffer of type `T`.
 *
 * @tparam T The data type of the elements in the decompressed buffer.
 * @param data A double pointer to the input/output buffer. The original buffer
 *             will be freed, and a new decompressed buffer will be allocated.
 * @param size A pointer to the size of the input/output buffer. This value will
 *             be updated with the size of the decompressed buffer.
 * @return The size of the decompressed buffer in bytes, or 0 if an error occurs.
 */
template<typename T>
std::size_t terse_to_hdf5_buffer(void** data, std::size_t* size) {
    struct Fixed_istreambuf : public std::streambuf {
        char* buffer;
        std::size_t buffer_size;

        Fixed_istreambuf(char* buffer, std::size_t size) : buffer(buffer), buffer_size(size) { setg(buffer, buffer, buffer + size); }

        std::streampos seekoff(std::streamoff off, std::ios_base::seekdir way, std::ios_base::openmode which = std::ios_base::in) override {
            switch (way) {
                case (std::ios_base::beg): setg(buffer, buffer + off, buffer + buffer_size); break;
                case (std::ios_base::end): setg(buffer, buffer + buffer_size + off, buffer + buffer_size); break;
                default: gbump(static_cast<int>(off));
            }
            return gptr() - eback();
        }

        std::streampos seekpos(std::streampos pos, std::ios_base::openmode which = std::ios_base::in) override {
            return seekoff(pos, std::ios_base::beg, which);
        }
    };

    std::span buffer(static_cast<char*>(*data), *size);
    Fixed_istreambuf iss_buffer(static_cast<char*>(buffer.data()), buffer.size());
    std::istream iss(&iss_buffer);
    jpa::Terse<jpa::Concurrent> terse_chunks(iss);
    std::size_t chunk_size = terse_chunks.size();
    jpa::Terse<jpa::Concurrent> terse_rest;
    if (!iss.eof()) terse_rest = jpa::Terse<jpa::Concurrent>(iss);
    std::size_t prolix_size = terse_chunks.size() * terse_chunks.number_of_frames() + terse_rest.size();
    void* new_buf = malloc(prolix_size * sizeof(T));
    if (!new_buf) {
        std::cerr << "Allocation of memory failed while expanding data in terse_to_hdf5_buffer"<< std::endl;
        return 0;
    }
    free(*data);
    *data = new_buf;
    std::span<T> prolix(static_cast<T*>(new_buf), prolix_size);
    terse_chunks.prolix(std::span(prolix.data(), chunk_size * terse_chunks.number_of_frames()));
    terse_rest.prolix(std::span(prolix.data() + chunk_size * terse_chunks.number_of_frames(), terse_rest.size()));
    return *size = prolix_size * sizeof(T);
}

/**
 * @brief Internal C++ implementation of the Terse HDF5 filter.
 *
 * This function handles both compression and decompression based on the
 * provided `flags`. It delegates to either `hdf5_buffer_to_terse` or
 * `terse_to_hdf5_buffer` depending on the `H5Z_FLAG_REVERSE` flag.
 *
 * @param flags HDF5 filter flags indicating whether to compress or decompress.
 * @param cd_nelmts The number of elements in `cd_values`.
 * @param cd_values Array containing filter parameters (e.g., data type code).
 * @param bufsize Pointer to the size of the input/output buffer.
 * @param buf Pointer to the input/output buffer.
 * @return The size of the processed buffer, or 0 if an error occurs.
 */
size_t Terse_filter_cpp(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], size_t *bufsize, void **buf) {
    if (!buf || !bufsize || cd_nelmts == 0 || cd_values == nullptr || cd_values[0] > 5) {
        std::cerr << "Invalid arguments provided to Terse_filter" << std::endl;
        return 0;
    }
    unsigned int data_type_code = cd_values[0];
    if (flags & H5Z_FLAG_REVERSE) switch (data_type_code) {
        case TYPE_CODE_INT16:  return terse_to_hdf5_buffer<int16_t> (buf, bufsize);
        case TYPE_CODE_UINT16: return terse_to_hdf5_buffer<uint16_t>(buf, bufsize);
        case TYPE_CODE_INT32:  return terse_to_hdf5_buffer<int32_t> (buf, bufsize);
        case TYPE_CODE_UINT32: return terse_to_hdf5_buffer<uint32_t>(buf, bufsize);
        case TYPE_CODE_INT8:   return terse_to_hdf5_buffer<int8_t>  (buf, bufsize);
        case TYPE_CODE_UINT8:  return terse_to_hdf5_buffer<uint8_t> (buf, bufsize);
    }
    else switch (data_type_code) {
        case TYPE_CODE_INT16:  return hdf5_buffer_to_terse<int16_t> (buf, bufsize, TERSE_DEFAULT_CHUNK_SIZE);
        case TYPE_CODE_UINT16: return hdf5_buffer_to_terse<uint16_t>(buf, bufsize, TERSE_DEFAULT_CHUNK_SIZE);
        case TYPE_CODE_INT32:  return hdf5_buffer_to_terse<int32_t> (buf, bufsize, TERSE_DEFAULT_CHUNK_SIZE);
        case TYPE_CODE_UINT32: return hdf5_buffer_to_terse<uint32_t>(buf, bufsize, TERSE_DEFAULT_CHUNK_SIZE);
        case TYPE_CODE_INT8:   return hdf5_buffer_to_terse<int8_t>  (buf, bufsize, TERSE_DEFAULT_CHUNK_SIZE);
        case TYPE_CODE_UINT8:  return hdf5_buffer_to_terse<uint8_t> (buf, bufsize, TERSE_DEFAULT_CHUNK_SIZE);
    }
    return 0;
}

/**
 * @brief Main C-code entry point for the Terse HDF5 filter.
 *
 * This function serves as the primary entry point for integrating the Terse
 * filter into the HDF5 library. It handles both compression and decompression,
 * delegating the operation to `Terse_filter_cpp`.
 *
 * @param flags HDF5 filter flags indicating whether to compress or decompress.
 *              `H5Z_FLAG_REVERSE` indicates decompression.
 * @param cd_nelmts The number of elements in `cd_values`.
 * @param cd_values Array containing filter parameters (e.g., data type code).
 * @param nbytes The size of the input buffer in bytes.
 * @param bufsize Pointer to the size of the input/output buffer.
 * @param buf Pointer to the input/output buffer.
 * @return The size of the processed buffer, or 0 if an error occurs.
 */
extern "C" size_t Terse_filter(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[],
                               size_t nbytes, size_t *bufsize, void **buf);extern "C" size_t Terse_filter(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[],
                               size_t nbytes, size_t *bufsize, void **buf) {
    return Terse_filter_cpp(flags, cd_nelmts, cd_values, bufsize, buf);
}

extern "C" herr_t register_terse_filter() {
   H5Z_class2_t filter_class = {
       H5Z_CLASS_T_VERS,
       TERSE_FILTER_ID,
       1,
       1,
       "TERSE",
       NULL,
       NULL,
       Terse_filter
   };
   return H5Zregister(&filter_class);
}

extern "C" const void* H5PLget_plugin_info(void) {
   static H5Z_class2_t filter_class = {
       H5Z_CLASS_T_VERS,
       TERSE_FILTER_ID,
       H5Z_FILTER_CONFIG_ENCODE_ENABLED | H5Z_FILTER_CONFIG_DECODE_ENABLED,
       1,
       "TERSE",
       NULL,
       NULL,
       Terse_filter
   };
   return static_cast<const void*>(&filter_class);
}

extern "C" H5PL_type_t H5PLget_plugin_type(void) {
   return H5PL_TYPE_FILTER;
}


