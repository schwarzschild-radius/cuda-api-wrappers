/**
 * @file memory.hpp
 *
 * @brief freestanding wrapper functions for working with CUDA's various
 * kinds of memory spaces, arranged into a relevant namespace hierarchy.
 */
#pragma once
#ifndef CUDA_API_WRAPPERS_MEMORY_HPP_
#define CUDA_API_WRAPPERS_MEMORY_HPP_

#include <cuda/api/error.hpp>
#include <cuda/api/constants.h>
#include <cuda/api/current_device.hpp>

#include <cuda_runtime.h> // needed, rather than cuda_runtime_api.h, e.g. for cudaMalloc

#include <memory>
#include <cstring> // for std::memset

namespace cuda {
namespace memory {

namespace mapped {

// This namespace regards memory appearing both on the device
// and on the host, with the regions mapped to each other.

// TODO: Perhaps make this an array of size 2 and use aspects to index it?
// Or maybe inherit a pair?
struct region_pair {

	enum : bool {
		is_portable_across_cuda_contexts   = true,
		isnt_portable_across_cuda_contexts = false
	};

	enum : bool {
		with_cpu_write_combining    = true,
		without_cpu_write_combining = false
	};

	struct allocation_options {
		bool portable_across_cuda_contexts;
		bool cpu_write_combining;
	};

	void* host_side;
	void* device_side;
	// size_t size_in_bytes; // common to both sides
	// allocation_options properties;

	// operator std::pair<void*, void*>() { return make_pair(host_side, device_side); }
	// size_t size() { return size_in_bytes; }
};

namespace detail {

inline unsigned make_cuda_host_alloc_flags(region_pair::allocation_options options) {
	return cudaHostAllocMapped &
		(options.portable_across_cuda_contexts ? cudaHostAllocPortable : 0) &
		(options.cpu_write_combining ? cudaHostAllocWriteCombined: 0);
}

} // namespace detail

} // namespace mapped
} // namespace memory


namespace memory {

namespace device {

namespace detail {

/**
 * Allocate memory on current device
 *
 * @param num_bytes amount of memory to allocate in bytes
 */
void* allocate(size_t num_bytes)
{
	void* allocated = nullptr;
	// Note: the typed cudaMalloc also takes its size in bytes, apparently,
	// not in number of elements
	auto status = cudaMalloc(&allocated, num_bytes);
	if (is_success(status) && allocated == nullptr) {
		// Can this even happen? hopefully not
		status = cudaErrorUnknown;
	}
	cuda::throw_if_error(status,
		"Failed allocating " + std::to_string(num_bytes) +
		" bytes of global memory on CUDA device " +
		std::to_string(cuda::device::current::get_id()));
	return allocated;
}

} // namespace detail

/**
 * Free a region of device-side memory which was allocated with @ref allocate.
 */
inline void free(void* ptr)
{
	auto result = cudaFree(ptr);
	cuda::throw_if_error(result, "Freeing device memory at 0x" + cuda::detail::ptr_as_hex(ptr));
}

/**
 * Allocate device-side memory on a CUDA device.
 *
 * @note The CUDA memory allocator guarantees alignment "suitabl[e] for any kind of variable"
 * (CUDA 9.0 Runtime API documentation), so probably at least 128 bytes.
 *
 * @throws cuda::runtime_error if allocation fails for any reason
 *
 * @param size_in_bytes the amount of memory to allocate
 * @return a pointer to the allocated stretch of memory (on the CUDA device)
 */
__host__ void* allocate(cuda::device::id_t device_id, size_t size_in_bytes)
{
	cuda::device::current::scoped_override_t<> set_device_for_this_scope(device_id);
	return memory::device::detail::allocate(size_in_bytes);
}

namespace detail {
struct allocator {
	// Allocates on the current device!
	void* operator()(size_t num_bytes) const { return detail::allocate(num_bytes); }
};
struct deleter {
	void operator()(void* ptr) const { cuda::memory::device::free(ptr); }
};
} // namespace detail

/**
 * @brief Sets all bytes in a region of memory to a fixed value
 *
 * @note The equivalent of @ref std::memset for CUDA device-side memory
 *
 * @param buffer_start position from where to start
 * @param byte_value value to set the memory region to
 * @param num_bytes size of the memory region in bytes
 */
inline void set(void* buffer_start, int byte_value, size_t num_bytes)
{
	auto result = cudaMemset(buffer_start, byte_value, num_bytes);
	cuda::throw_if_error(result, "memsetting an on-device buffer");
}

/**
 * @brief Sets all bytes in a region of memory to 0 (zero)
 *
 * @param buffer_start position from where to start
 * @param num_bytes size of the memory region in bytes
 */
inline void zero(void* buffer_start, size_t num_bytes)
{
	return set(buffer_start, 0, num_bytes);
}

} // namespace device

/**
 * Synchronously copies data between memory spaces or within a memory space.
 *
 * @note Since we assume Compute Capability >= 2.0, all devices support the
 * Unified Virtual Address Space, so the CUDA driver can determine, for each pointer,
 * where the data is located, and one does not have to specify this.
 *
 * @param destination A pointer to a memory region of size @p num_bytes, either in
 * host memory or on any CUDA device's global memory
 * @param source A pointer to a a memory region of size @p num_bytes, either in
 * host memory or on any CUDA device's global memory
 * @param num_bytes The number of bytes to copy from @p source to @p destination
 */
inline void copy(void *destination, const void *source, size_t num_bytes)
{
	auto result = cudaMemcpy(destination, source, num_bytes, cudaMemcpyDefault);
	if (is_failure(result)) {
		std::string error_message("Synchronously copying data");
		// TODO: Determine whether it was from host to device, device to host etc and
		// add this information to the error string
		throw_if_error(result, error_message);
	}
}

/**
 * Synchronously copies a single (typed) value between memory spaces or within a memory space.
 *
 * @param destination a value residing either in host memory or on any CUDA
 * device's global memory
 * @param source a value residing either in host memory or on any CUDA
 * device's global memory
 */
template <typename T>
inline void copy_single(T& destination, const T& source)
{
	copy(&destination, &source, sizeof(T));
}

namespace async {

/**
 * Asynchronously copies data between memory spaces or within a memory space.
 *
 * @note Since we assume Compute Capability >= 2.0, all devices support the
 * Unified Virtual Address Space, so the CUDA driver can determine, for each pointer,
 * where the data is located, and one does not have to specify this.
 *
 * @note asynchronous version of @ref memory::copy
 *
 * @param destination A pointer to a memory region of size @p num_bytes, either in
 * host memory or on any CUDA device's global memory
 * @param source A pointer to a a memory region of size @p num_bytes, either in
 * host memory or on any CUDA device's global memory
 * @param num_bytes The number of bytes to copy from @p source to @p destination
 */
inline void copy(void *destination, const void *source, size_t num_bytes, stream::id_t stream_id)
{
	auto result = cudaMemcpyAsync(destination, source, num_bytes, cudaMemcpyDefault, stream_id);
	if (is_failure(result)) {
		std::string error_message("Scheduling a memory copy on stream " + cuda::detail::ptr_as_hex(stream_id));
		// TODO: Determine whether it was from host to device, device to host etc and
		// add this information to the error string
		throw_if_error(result, error_message);
	}
}

/**
 * Synchronously copies a single (typed) value between memory spaces or within a memory space.
 *
 * @note asynchronous version of @ref memory::copy_single
 *
 * @param destination a value residing either in host memory or on any CUDA
 * device's global memory
 * @param source a value residing either in host memory or on any CUDA
 * device's global memory
 */
template <typename T>
inline void copy_single(T& destination, const T& source, stream::id_t stream_id)
{
	copy(&destination, &source, sizeof(T), stream_id);
}

} // namespace async

namespace device {

namespace async {

inline void set(void* buffer_start, int byte_value, size_t num_bytes, stream::id_t stream_id)
{
	auto result = cudaMemsetAsync(buffer_start, byte_value, num_bytes, stream_id);
	throw_if_error(result, "memsetting an on-device buffer");
}

inline void zero(void* buffer_start, size_t num_bytes, stream::id_t stream_id)
{
	return set(buffer_start, 0, num_bytes, stream_id);
}

} // namespace async

} // namespace device

namespace host {

/**
 * Allocate pinned host memory
 *
 * @note "Pinned" memory is excepted from virtual memory swapping-out,
 * and is allocated in contiguous physical RAM addresses, making it
 * possible to copy to and from it to the the GPU using DMA without
 * assistance from the GPU. Typically for PCIe 3.0, the effective
 * bandwidth is twice as fast as copying from or to naively-allocated
 * host memory.
 *
 * @throws cuda::runtime_error if allocation fails for any reason
 *
 * @todo Consider a variant of this supporting the cudaHostAlloc flags
 *
 * @param size_in_bytes the amount of memory to allocate, in bytes
 * @return a pointer to the allocated stretch of memory
 */
inline void* allocate(size_t size_in_bytes /* write me:, bool recognized_by_all_contexts */)
{
	void* allocated = nullptr;
	// Note: the typed cudaMallocHost also takes its size in bytes, apparently, not in number of elements
	auto result = cudaMallocHost(&allocated, size_in_bytes);
	if (is_success(result) && allocated == nullptr) {
		// Can this even happen? hopefully not
		result = cudaErrorUnknown;
	}
	cuda::throw_if_error(result, "Failed allocating " + std::to_string(size_in_bytes) + " bytes of host memory");
	return allocated;
}

/**
 * Free a region of pinned host memory which was allocated with @ref allocate.
 */
inline void free(void* host_ptr)
{
	auto result = cudaFreeHost(host_ptr);
	cuda::throw_if_error(result, "Freeing pinned host memory at 0x" + cuda::detail::ptr_as_hex(host_ptr));
}

namespace detail {

struct allocator {
	void* operator()(size_t num_bytes) const { return cuda::memory::host::allocate(num_bytes); }
};
struct deleter {
	void operator()(void* ptr) const { cuda::memory::host::free(ptr); }
};


/**
 * @brief Makes a preallocated memory region behave as though it were allocated with @ref host::allocate.
 *
 * Page-locks the memory range specified by ptr and size and maps it for the device(s) as specified by
 * flags. This memory range also is added to the same tracking mechanism as cudaHostAlloc() to
 * automatically accelerate calls to functions such as cudaMemcpy().
 *
 * @param ptr A pre-allocated stretch of host memory
 * @param size the size in bytes the memory region to register/pin
 * @param flags
 */
inline void register_(void *ptr, size_t size, unsigned flags)
{
	auto result = cudaHostRegister(ptr, size, flags);
	cuda::throw_if_error(result,
		"Could not register a region of page-locked host memory");
}

} // namespace detail

enum mapped_io_space : bool {
	is_mapped_io_space               = true,
	is_not_mapped_io_space           = false
};

enum mapped_into_device_memory : bool {
	is_mapped_into_device_memory     = true,
	is_not_mapped_into_device_memory = false
};

enum accessibility_on_all_devices : bool {
	is_accessible_on_all_devices     = true,
	is_not_accessible_on_all_devices = false
};


// Can't use register(), since that's a reserved word
inline void register_(void *ptr, size_t size,
	bool register_mapped_io_space,
	bool map_into_device_space,
	bool make_device_side_accesible_to_all)
{
	return detail::register_(
		ptr, size,
		  register_mapped_io_space ? cudaHostRegisterIoMemory : 0
		| map_into_device_space ? cudaHostRegisterMapped : 0
		| make_device_side_accesible_to_all ? cudaHostRegisterPortable : 0
	);
}

inline void register_(void *ptr, size_t size)
{
	detail::register_(ptr, size, cudaHostRegisterDefault);
}

// the CUDA API calls this "unregister", but that's semantically
// inaccurate. The registration is not undone, rolled back, it's
// just ended
inline void deregister(void *ptr)
{
	auto result = cudaHostUnregister(ptr);
	cuda::throw_if_error(result,
		"Could not unregister the memory segment starting at address *a");
}

inline void set(void* buffer_start, int byte_value, size_t num_bytes)
{
	std::memset(buffer_start, byte_value, num_bytes);
	// TODO: Error handling?
}

inline void zero(void* buffer_start, size_t num_bytes)
{
	return set(buffer_start, 0, num_bytes);
	// TODO: Error handling?
}


} // namespace host

namespace managed {

enum class initial_visibility_t {
	to_all_devices,
	to_supporters_of_concurrent_managed_access,
};

namespace detail {

void* allocate(
	size_t                num_bytes,
	initial_visibility_t  initial_visibility = initial_visibility_t::to_all_devices)
{
	void* allocated = nullptr;
	auto flags = (initial_visibility == initial_visibility_t::to_all_devices) ?
		cudaMemAttachGlobal : cudaMemAttachHost;
	// Note: Despite the templating by T, the size is still in bytes,
	// not in number of T's
	auto status = cudaMallocManaged(&allocated, num_bytes, flags);
	if (is_success(status) && allocated == nullptr) {
		// Can this even happen? hopefully not
		status = (status_t) status::unknown;
	}
	cuda::throw_if_error(status,
		"Failed allocating " + std::to_string(num_bytes) + " bytes of managed CUDA memory");
	return allocated;
}

/**
 * Free a region of pinned host memory which was allocated with @ref allocate.
 */
inline void free(void* ptr)
{
	auto result = cudaFree(ptr);
	cuda::throw_if_error(result, "Freeing managed memory at 0x" + cuda::detail::ptr_as_hex(ptr));
}

template <initial_visibility_t InitialVisibility = initial_visibility_t::to_all_devices>
struct allocator {
	// Allocates on the current device!
	void* operator()(size_t num_bytes) const
	{
		return detail::allocate(num_bytes, InitialVisibility);
	}
};
struct deleter {
	void operator()(void* ptr) const { cuda::memory::device::free(ptr); }
};

} // namespace detail

/**
 * @brief Allocate a a region of managed memory, accessible with the same
 * address on the host and on CUDA devices
 *
 * CUDA "managed memory" means being able to use the same address on the host
 * and on CUDA devices to refer to different memory regions, one in host
 * memory and others in global device memory. CUDA "manages" these regions,
 * i.e. migrates data between CUDA devices and host memory when its been written
 * somewhere and needs to be read elsewhere.
 * See @url https://devblogs.nvidia.com/parallelforall/unified-memory-in-cuda-6/
 *
 * @note "managed memory" is similar to "mapped memory" in the propagation
 * of changes from the host to the device and back as necessary, but differs
 * from it in that "mapped memory" involves just a single device; and that
 * with "mapped memory" the host-side and device-side addresses are different
 *
 * @param device_id the initial device which is likely to access the managed
 * memory region (and which will certainly have actually allocated for it)
 * @param num_bytes size of each of the regions of memory to allocate
 * @param initial_visibility will the allocated region be visible, using the
 * common address, to all CUDA device (= more overhead, more work for the CUDA
 * runtime) or just to those devices with some hardware features to assist in
 * this task (= less overhead)?
 */

void* allocate(
	cuda::device::id_t    device_id,
	size_t                num_bytes,
	initial_visibility_t  initial_visibility = initial_visibility_t::to_all_devices)
{
	cuda::device::current::scoped_override_t<> set_device_for_this_scope(device_id);
	return detail::allocate(num_bytes, initial_visibility);
}

/**
 * Free a managed memory region (host-side and device-side regions on all devices
 * where it was allocated, all with the same address) which was allocated with
 * @ref allocate.
 */
inline void free(void* managed_ptr)
{
	auto result = cudaFree(managed_ptr);
	cuda::throw_if_error(result,
		"Freeing managed memory (host and device regions) at address 0x"
		+ cuda::detail::ptr_as_hex(managed_ptr));
}

} // namespace managed

namespace mapped {

namespace detail {

/**
 * Allocates on the current device.
 */
inline region_pair allocate(
	size_t                           size_in_bytes,
	region_pair::allocation_options  options)
{
	region_pair allocated;
	auto flags = cuda::memory::mapped::detail::make_cuda_host_alloc_flags(options);
	// Note: the typed cudaHostAlloc also takes its size in bytes, apparently,
	// not in number of elements
	auto status = cudaHostAlloc(&allocated.host_side, size_in_bytes, flags);
	if (is_success(status) && (allocated.host_side == nullptr)) {
		// Can this even happen? hopefully not
		status = cudaErrorUnknown;
	}
	if (is_success(status)) {
		auto get_device_pointer_flags = 0u; // see the CUDA runtime documentation
		status = cudaHostGetDevicePointer(
			&allocated.device_side,
			allocated.host_side,
			get_device_pointer_flags);
	}
	throw_if_error(status,
		"Failed allocating a mapped pair of memory regions of size " + std::to_string(size_in_bytes)
			+ " bytes of global memory on device " + std::to_string(cuda::device::current::get_id()));
	return allocated;
}

} // namespace detail

/**
 * Allocate a pair of mapped memory regions, one in host memory and one
 * in the global memory of a CUDA device, which the CUDA runtime will keep
 * syncrhonized
 *
 * @note "managed memory" is similar to "mapped memory" in the propagation
 * of changes from the host to the device and back as necessary, but differs
 * from it in that "mapped memory" involves just a single device; and that
 * with "mapped memory" the host-side and device-side addresses are different
 *
 * @param device_id The device on which to allocate the device-side region
 * @param size_in_bytes size of each of the memory region
 * @param options see @ref region_pair::allocation_options
 * @return a structure for the pair of allocated regions
 */
inline region_pair allocate(
	cuda::device::id_t               device_id,
	size_t                           size_in_bytes,
	region_pair::allocation_options  options = {
		region_pair::isnt_portable_across_cuda_contexts,
		region_pair::without_cpu_write_combining }
	)
{
	cuda::device::current::scoped_override_t<> set_device_for_this_scope(device_id);
	return detail::allocate(size_in_bytes, options);
}

/**
 * Free a pair of mapped memory regions allocated with @ref allocate.
 */
inline void free(region_pair pair)
{
	auto result = cudaFreeHost(pair.host_side);
	cuda::throw_if_error(result, "Could not free mapped memory region pair.");
}

/**
 * Free a pair of mapped memory regions allocated with @ref allocate
 *
 * @param ptr a pointer to one of the mapped regions (can be either
 * the device-side or the host-side)
 */
inline void free_region_pair_of(void* ptr)
{
	cudaPointerAttributes attributes;
	auto result = cudaPointerGetAttributes(&attributes, ptr);
	cuda::throw_if_error(result,
		"Could not obtain the properties for the pointer"
		", being necessary for freeing the region pair it's (supposedly) "
		"associated with.");
	cudaFreeHost(attributes.hostPointer);
}

/**
 * Determines whether a given stretch of memory was allocated as part of
 * a mapped pair of host and device memory regions
 *
 * @note mostly for debugging purposes
 *
 * @param ptr the region to check
 * @return true iff the region was allocated as one side of a mapped memory region pair
 */
inline bool is_part_of_a_region_pair(void* ptr)
{
	cudaPointerAttributes attributes;
	auto result = cudaPointerGetAttributes(&attributes, ptr);
	cuda::throw_if_error(result, "Could not obtain device pointer attributes");
#ifdef DEBUG
	auto self_copy = (attributes.memoryType == cudaMemoryTypeHost) ?
		attributes.hostPointer : attributes.devicePointer ;
	if (self_copy != ptr) {
		throw runtime_error(cudaErrorUnknown, "Inconsistent data obtained from the CUDA runtime API");
	}
#endif
	auto corresponding_buffer_ptr =
		(attributes.memoryType == cudaMemoryTypeHost) ?
		attributes.devicePointer : attributes.hostPointer;
	return (corresponding_buffer_ptr != nullptr);
}

} // namespace mapped

} // namespace memory

} // namespace cuda

#endif /* CUDA_API_WRAPPERS_MEMORY_HPP_ */
