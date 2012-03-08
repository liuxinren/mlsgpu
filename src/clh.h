/**
 * @file
 */

#ifndef CLH_H
#define CLH_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <boost/program_options.hpp>
#include <boost/noncopyable.hpp>
#include <vector>
#include <string>
#include <map>
#include <CL/cl.hpp>
#include <tr1/cstdint>

/// OpenCL helper functions
namespace CLH
{

namespace detail
{

class MemoryMapping : public boost::noncopyable
{
private:
    cl::Memory memory;      ///< Memory object to unmap on destruction
    cl::CommandQueue queue; ///< Privately allocated command queue
    void *ptr;              ///< Mapped pointer

protected:
    MemoryMapping(const cl::Memory &memory, const cl::Device &device);
    ~MemoryMapping();

    void setPointer(void *ptr) { this->ptr = ptr; }
    const cl::CommandQueue &getQueue() const { return queue; }
    const cl::Memory &getMemory() const { return memory; }

public:
    void *get() const { return ptr; }
};

} // namespace detail

/**
 * RAII wrapper around mapping and unmapping a buffer.
 * It only handles synchronous mapping and unmapping.
 */
class BufferMapping : public detail::MemoryMapping
{
public:
    BufferMapping(const cl::Buffer &buffer, const cl::Device &device, cl_map_flags flags, ::size_t offset, ::size_t size);

    const cl::Buffer &getBuffer() const { return static_cast<const cl::Buffer &>(getMemory()); }
};

/**
 * RAII wrapper around mapping and unmapping an image.
 * It only handles synchronous mapping and unmapped.
 */
class ImageMapping : public detail::MemoryMapping
{
public:
    ImageMapping(const cl::Image &image, const cl::Device &device, cl_map_flags flags,
                 const cl::size_t<3> &origin, const cl::size_t<3> &region,
                 ::size_t *rowPitch, ::size_t *slicePitch);

    const cl::Image &getImage() const { return static_cast<const cl::Image &>(getMemory()); }
};

/**
 * Represents the resources required or consumed by an algorithm class.
 */
class ResourceUsage
{
private:
    std::tr1::uint64_t maxMemory;     ///< Largest single memory allocation
    std::tr1::uint64_t totalMemory;   ///< Sum of all allocations
    std::size_t imageWidth;           ///< Maximum image width used (0 if no images)
    std::size_t imageHeight;          ///< Maximum image height used (0 if no images)

public:
    ResourceUsage() : maxMemory(0), totalMemory(0), imageWidth(0), imageHeight(0) {}

    /// Indicate that memory for a buffer is required.
    void addBuffer(std::tr1::uint64_t bytes);

    /// Indicate that memory for a 2D image is required.
    void addImage(std::size_t width, std::size_t height, std::size_t bytesPerPixel);

    /**
     * Computes the combined requirements given the individual requirements for two
     * steps. This assumes that the steps are active simultaneously, and hence that
     * totals must be added.
     */
    ResourceUsage operator+(const ResourceUsage &r) const;

    /**
     * Adds @a n copies of the resource.
     */
    ResourceUsage operator*(unsigned int n) const;

    /// Retrieve the maximum single allocation size required.
    std::tr1::uint64_t getMaxMemory() const { return maxMemory; }
    /// Retrieve the maximum total memory required.
    std::tr1::uint64_t getTotalMemory() const { return totalMemory; }
    /// Retrieve the largest image width required (0 if no images).
    std::size_t getImageWidth() const { return imageWidth; }
    /// Retrieve the largest image height required (0 if no images).
    std::size_t getImageHeight() const { return imageHeight; }
};

/// Option names for OpenCL options
namespace Option
{
const char * const device = "cl-device";
const char * const gpu = "cl-gpu";
const char * const cpu = "cl-cpu";
} // namespace Option

/**
 * Append program options for selecting an OpenCL device.
 *
 * The resulting variables map can be passed to @ref findDevice.
 */
void addOptions(boost::program_options::options_description &desc);

/**
 * Pick an OpenCL device based on command-line options.
 *
 * If more than one device matches the criteria, GPU devices are preferred.
 * If there is no exact match for the device name, a prefix will be accepted.
 *
 * @return A device matching the command-line options, or @c NULL if none matches.
 */
cl::Device findDevice(const boost::program_options::variables_map &vm);

/**
 * Create an OpenCL context suitable for use with a device.
 */
cl::Context makeContext(const cl::Device &device);

/**
 * Build a program for potentially multiple devices.
 *
 * If compilation fails, the build log will be emitted to the error log.
 *
 * @param context         Context to use for building.
 * @param devices         Devices to build for.
 * @param filename        File to load (relative to current directory).
 * @param defines         Defines that will be set before the source is preprocessed.
 * @param options         Extra compilation options.
 *
 * @throw std::invalid_argument if the file could not be opened.
 * @throw cl::Error if the program could not be compiled.
 */
cl::Program build(const cl::Context &context, const std::vector<cl::Device> &devices,
                  const std::string &filename, const std::map<std::string, std::string> &defines = std::map<std::string, std::string>(),
                  const std::string &options = "");

/**
 * Build a program for all devices associated with a context.
 *
 * This is a convenience wrapper for the form that takes an explicit device
 * list.
 */
cl::Program build(const cl::Context &context,
                  const std::string &filename, const std::map<std::string, std::string> &defines = std::map<std::string, std::string>(),
                  const std::string &options = "");

/**
 * Implementation of clEnqueueMarkerWithWaitList which can be used in OpenCL
 * 1.1. It differs from the OpenCL 1.2 function in several ways:
 *  - If no input events are passed, it does not wait for anything (other than as
 *    constrained by an in-order queue), rather than waiting for all previous
 *    work.
 *  - If exactly one input event is passed, it may return this event (with
 *    an extra reference) instead of creating a new one.
 *  - It may choose to wait on all previous events in the command queue.
 *    Thus, if your algorithm depends on it not doing so (e.g. you've used
 *    user events to create dependencies backwards in time) it may cause
 *    a deadlock.
 *  - It is legal for @a event to be @c NULL (in which case the marker is
 *    still enqueued, so that if the wait list contained events from other
 *    queues then a barrier on this queue would happen-after those events).
 */
cl_int enqueueMarkerWithWaitList(const cl::CommandQueue &queue,
                                 const std::vector<cl::Event> *events,
                                 cl::Event *event);

/**
 * Extension of @c cl::CommandQueue::enqueueReadBuffer that allows the
 * size to be zero.
 */
cl_int enqueueReadBuffer(
    const cl::CommandQueue &queue,
    const cl::Buffer &buffer,
    cl_bool blocking,
    std::size_t offset,
    std::size_t size,
    void *ptr,
    const std::vector<cl::Event> *events = NULL,
    cl::Event *event = NULL);

/**
 * Extension of @c cl::CommandQueue::enqueueWriteBuffer that allows the
 * size to be zero.
 */
cl_int enqueueWriteBuffer(
    const cl::CommandQueue &queue,
    const cl::Buffer &buffer,
    cl_bool blocking,
    std::size_t offset,
    std::size_t size,
    const void *ptr,
    const std::vector<cl::Event> *events = NULL,
    cl::Event *event = NULL);

/**
 * Extension of @c cl::CommandQueue::enqueueCopyBuffer that allows the size
 * to be zero.
 */
cl_int enqueueCopyBuffer(
    const cl::CommandQueue &queue,
    const cl::Buffer &src,
    const cl::Buffer &dst,
    std::size_t srcOffset,
    std::size_t dstOffset,
    std::size_t size,
    const std::vector<cl::Event> *events = NULL,
    cl::Event *event = NULL);

/**
 * Extension of @c cl::CommandQueue::enqueueNDRangeKernel that allows the
 * number of work-items to be zero.
 */
cl_int enqueueNDRangeKernel(
    const cl::CommandQueue &queue,
    const cl::Kernel &kernel,
    const cl::NDRange &offset,
    const cl::NDRange &global,
    const cl::NDRange &local = cl::NullRange,
    const std::vector<cl::Event> *events = NULL,
    cl::Event *event = NULL);

} // namespace CLH

#endif /* !CLH_H */
