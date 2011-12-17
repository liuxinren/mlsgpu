/**
 * @file
 *
 * Implementation of @ref SplatTree using OpenCL buffers for the backing store.
 */

#ifndef SPLATTREE_CL_H
#define SPLATTREE_CL_H

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <CL/cl.hpp>
#include <boost/noncopyable.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include "splat_tree.h"
#include "src/clh.h"
#include "grid.h"
#include "clcpp/clcpp.h"

/**
 * Concrete implementation of @ref SplatTree that stores the data
 * in OpenCL buffers. It does not actually derive from @ref SplatTree because
 * it does not re-use the building code, but it presents similar interfaces.
 *
 * To ease implementation, levels are numbered backwards i.e. level 0 is the
 * largest, finest-grained level, and the last level is 1x1x1.
 */
class SplatTreeCL
{
public:
    /**
     * Type used to represent values in the command table.
     * It needs enough bits to represent splat values and jump values.
     */
    typedef std::tr1::int32_t command_type;

    /**
     * Type used to represent indices into the cells, and also for
     * sort keys.
     */
    typedef std::tr1::uint32_t code_type;

private:
    Grid grid;

    /// OpenCL context used to create buffers.
    cl::Context context;

    /// Program containing the internal kernels for building
    cl::Program program;

    /**
     * @name
     * @{
     * Kernels implementing the internal operations.
     */
    cl::Kernel writeEntriesKernel, countCommandsKernel, writeSplatIdsKernel, writeStartKernel;
    cl::Kernel fillKernel;
    /** @} */

    /**
     * @name
     * @{
     * Backing storage for the octree.
     * @see SplatTree.
     */
    cl::Buffer splats;
    cl::Buffer start;
    cl::Buffer commands;
    /** @} */

    /**
     * @name
     * @{
     * Intermediate data structures used while building the octree.
     *
     * These are never deleted, so that the memory can be recycled each
     * time the octree is regenerated.
     */
    cl::Buffer commandMap;   ///< Maps sorted entries to positions in the command array
    cl::Buffer jumpPos;      ///< Position in command array of jump command for each key (-1 if not present)
    cl::Buffer entryKeys;    ///< Sort keys for entries
    cl::Buffer entryValues;  ///< Splat IDs for entries
    /** @} */

    std::size_t maxSplats;   ///< Maximum splats for which memory has been allocated
    std::size_t maxLevels;   ///< Maximum levels for which memory has been allocated

    std::size_t numSplats;   ///< Number of splats in the octree
    std::vector<std::size_t> levelOffsets; ///< Start of each level in compacted arrays

    clcpp::Radixsort sort;   ///< Sorter for sorting the entries
    clcpp::Scan scan;        ///< Scanner for computing @ref commandMap

    /// Wrapper to call @ref writeEntries
    void enqueueWriteEntries(const cl::CommandQueue &queue,
                             const cl::Buffer &keys,
                             const cl::Buffer &values,
                             const cl::Buffer &splats,
                             command_type numSplats,
                             const Grid &grid,
                             std::size_t numLevels,
                             std::vector<cl::Event> *events,
                             cl::Event *event);

    /// Wrapper to call @ref countCommands
    void enqueueCountCommands(const cl::CommandQueue &queue,
                              const cl::Buffer &indicator,
                              const cl::Buffer &keys,
                              command_type numKeys,
                              std::vector<cl::Event> *events,
                              cl::Event *event);

    /// Wrapper to call @ref writeSplatIds
    void enqueueWriteSplatIds(const cl::CommandQueue &queue,
                              const cl::Buffer &commands,
                              const cl::Buffer &start,
                              const cl::Buffer &jumpPos,
                              const cl::Buffer &commandMap,
                              const cl::Buffer &keys,
                              const cl::Buffer &splatIds,
                              command_type numEntries,
                              std::vector<cl::Event> *events,
                              cl::Event *event);

    /// Wrapper to call @ref writeStart
    void enqueueWriteStart(const cl::CommandQueue &queue,
                           const cl::Buffer &start,
                           const cl::Buffer &commands,
                           const cl::Buffer &jumpPos,
                           code_type curOffset,
                           code_type prevOffset,
                           code_type numCodes,
                           std::vector<cl::Event> *events,
                           cl::Event *event);

    /// Wrapper to call @ref fill
    void enqueueFill(const cl::CommandQueue &queue,
                     const cl::Buffer &buffer,
                     std::size_t offset,
                     std::size_t elements,
                     command_type value,
                     std::vector<cl::Event> *events,
                     cl::Event *event);

public:
    /**
     * Constructor. This allocates the maximum supported sizes for all the
     * buffers necessary, but does not populate them.
     *
     * @param context   OpenCL context used to create buffers, images etc.
     * @param maxLevels Maximum number of octree levels (maximum dimension is 2^@a maxLevels).
     * @param maxSplats Maximum number of splats supported.
     */
    SplatTreeCL(const cl::Context &context, std::size_t maxLevels, std::size_t maxSplats);

    /**
     * Asynchronously builds the octree, discarding any previous contents.
     *
     * This must not be called while either a previous #enqueueBuild is still in
     * progress, or while the octree is being traversed.
     *
     * @param queue         The command queue for the building operations.
     * @param splats        The splats to put in the octree.
     * @param numSplats     The size of the @a splats array.
     * @param grid          The octree sampling grid.
     * @param blockingCopy  If true, the @a splats array can be reused on return.
     *                      Otherwise, one must wait for @a uploadEvent.
     * @param events        Events to wait for (or @c NULL).
     * @param[out] uploadEvent   Event that fires when @a splats may be reused (or @c NULL).
     * @param[out] event         Event that fires when the octree is ready to use (or @c NULL).
     *
     * @pre
     * - @a grid has no more than 2^(maxLevels - 1) elements in any direction.
     * - @a numSplats is less than @a maxSplats.
     * - @a splats is not @c NULL.
     */
    void enqueueBuild(const cl::CommandQueue &queue,
                      const Splat *splats, std::size_t numSplats,
                      const Grid &grid, bool blockingCopy,
                      const std::vector<cl::Event> *events = NULL,
                      cl::Event *uploadEvent = NULL, cl::Event *event = NULL);

    /**
     * @name Getters for the buffers and images needed to use the octree.
     * These can be called at any time, and remain valid across a call to
     * @ref enqueueBuild. However, the contents will only be valid when
     * @ref enqueueBuild has completed.
     * @see @ref processCorners.
     * @{
     */
    const cl::Buffer &getSplats() const { return splats; }
    const cl::Buffer &getCommands() const { return commands; }
    const cl::Buffer &getStart() const { return start; }
    /**
     * @}
     */

    /// Get the number of levels currently in the octree.
    std::size_t getNumLevels() const { return levelOffsets.size(); }
};

#endif /* !SPLATTREE_CL_H */
