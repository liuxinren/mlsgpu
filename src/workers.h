/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Collection of classes for doing specific steps from the main program.
 */

#ifndef WORKERS_H
#define WORKERS_H

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/locks.hpp>
#include <boost/noncopyable.hpp>
#include <boost/foreach.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/ptr_container/ptr_map.hpp>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <CL/cl.hpp>
#include "splat_tree_cl.h"
#include "marching.h"
#include "mls.h"
#include "mesh.h"
#include "mesher.h"
#include "mesh_filter.h"
#include "grid.h"
#include "progress.h"
#include "work_queue.h"
#include "bucket.h"
#include "splat.h"
#include "splat_set.h"
#include "clh.h"
#include "errors.h"
#include "statistics.h"
#include "allocator.h"
#include "worker_group.h"
#include "timeplot.h"

class MesherGroup;

class MesherGroupBase
{
public:
    struct WorkItem
    {
        MesherWork work;
        CircularBuffer::Allocation alloc; ///< Allocation backing the mesh data
    };

    class Worker : public WorkerBase
    {
    private:
        MesherGroup &owner;

    public:
        typedef void result_type;

        explicit Worker(MesherGroup &owner);
        void operator()(WorkItem &work);
    };
};

/**
 * Object for handling asynchronous meshing. It always uses one consumer thread, since
 * the operation is fundamentally not thread-safe. However, there may be multiple
 * producers.
 */
class MesherGroup : protected MesherGroupBase,
    public WorkerGroup<MesherGroupBase::WorkItem, MesherGroupBase::Worker, MesherGroup>
{
public:
    typedef MesherGroupBase::WorkItem WorkItem;

    /// Set the functor to use for processing data received from the output functor.
    void setInputFunctor(const MesherBase::InputFunctor &input) { this->input = input; }

    boost::shared_ptr<WorkItem> get(Timeplot::Worker &tworker, std::size_t size);

    /**
     * Constructor.
     *
     * @param memMesh Memory (in bytes) to allocate for holding queued mesh data.
     */
    explicit MesherGroup(const std::size_t memMesh);
private:
    MesherBase::InputFunctor input;
    CircularBuffer meshBuffer;

    friend class MesherGroupBase::Worker;

    void outputFunc(
        Timeplot::Worker &tworker,
        const ChunkId &chunkId,
        const cl::CommandQueue &queue,
        const DeviceKeyMesh &mesh,
        const std::vector<cl::Event> *events,
        cl::Event *event);
};


class DeviceWorkerGroup;

class DeviceWorkerGroupBase
{
protected:
    /**
     * Maximum size we will use for the distance field image. This is set to minimum
     * maximum for @c CL_DEVICE_IMAGE2D_MAX_HEIGHT.
     */
    static const int MAX_IMAGE_HEIGHT = 8192;

    /**
     * Compute a @a maxSwath value to pass to @ref Marching. If the returned
     * value is @a N, then it is guaranteed that
     * - @a N is a multiple of @a zAlign.
     * - @a N + 1 times @c roundUp(@a y, @a yAlign) is at most @a yMax.
     * - @a N is as large as possible given these constraints.
     *
     * However, if this would require a return value of 0, @a zAlign is returned instead.
     */
    static Grid::size_type computeMaxSwathe(
        Grid::size_type yMax, Grid::size_type y, Grid::size_type yAlign, Grid::size_type zAlign);

public:
    /// Data about a single bucket.
    struct SubItem
    {
        ChunkId chunkId;               ///< Chunk owning this item
        Grid grid;                     ///< Grid area containing the bucket (pre-transformed)
        std::size_t firstSplat;        ///< Index of first splat in device buffer
        std::size_t numSplats;         ///< Number of splats in the bucket
        std::size_t progressSplats;    ///< Splats to count towards the progress meter
    };

    /// Data about multiple buckets that share a single CL buffer.
    struct WorkItem
    {
        /// Data for individual buckets
        Statistics::Container::vector<SubItem> subItems;
        cl::Buffer splats;             ///< Backing store for splats
        cl::Event copyEvent;           ///< Event signaled when the splats are ready to use on device

        WorkItem(const cl::Context &context, std::size_t maxItemSplats)
            : subItems("mem.DeviceWorkerGroup.subItems"),
            splats(context, CL_MEM_READ_WRITE, maxItemSplats * sizeof(Splat))
        {
        }
    };

    class Worker : public WorkerBase
    {
    private:
        DeviceWorkerGroup &owner;

        const cl::CommandQueue queue;
        SplatTreeCL tree;
        MlsFunctor input;
        Marching marching;
        ScaleBiasFilter scaleBias;
        MeshFilterChain filterChain;

    public:
        typedef void result_type;

        Worker(
            DeviceWorkerGroup &owner,
            const cl::Context &context, const cl::Device &device,
            int levels, float boundaryLimit,
            MlsShape shape, int idx);

        void start();
        void operator()(WorkItem &work);
    };
};

/**
 * Does the actual OpenCL calls necessary to compute the mesh and write
 * it to the @ref MesherBase class. It pulls bins of work off a queue,
 * which contains pre-bucketed splats.
 */
class DeviceWorkerGroup :
    protected DeviceWorkerGroupBase,
    public WorkerGroup<DeviceWorkerGroupBase::WorkItem, DeviceWorkerGroupBase::Worker, DeviceWorkerGroup>
{
public:
    /**
     * Functor that generates an output function given the current chunk ID and
     * worker. This is used to abstract the downstream worker group class.
     *
     * @see @ref DeviceWorkerGroup::DeviceWorkerGroup
     */
    typedef boost::function<Marching::OutputFunctor(const ChunkId &, Timeplot::Worker &)> OutputGenerator;

private:
    typedef WorkerGroup<DeviceWorkerGroupBase::WorkItem, DeviceWorkerGroupBase::Worker, DeviceWorkerGroup> Base;

    ProgressMeter *progress;
    OutputGenerator outputGenerator;

    Grid fullGrid;
    const cl::Context context;
    const cl::Device device;
    const std::size_t maxBucketSplats;  ///< Maximum splats in a single bucket
    const Grid::size_type maxCells;
    const std::size_t meshMemory;
    const int subsampling;

    cl::CommandQueue copyQueue;   ///< Queue for transferring data to the device

    /// Pool of unused buffers to be recycled
    WorkQueue<boost::shared_ptr<WorkItem> > itemPool;

    /// Mutex held while signaling @ref popCondition
    boost::mutex *popMutex;

    /// Condition signaled when items are added to the pool (may be @c NULL)
    boost::condition_variable *popCondition;

    /// Number of spare splats in device buffers.
    std::size_t unallocated_;
    /// Mutex protecting @ref unallocated_.
    boost::mutex unallocatedMutex;

    friend class DeviceWorkerGroupBase::Worker;

public:
    typedef DeviceWorkerGroupBase::WorkItem WorkItem;
    typedef DeviceWorkerGroupBase::SubItem SubItem;

    /**
     * Constructor.
     *
     * @param numWorkers         Number of worker threads to use (each with a separate OpenCL queue and state)
     * @param spare              Number of extra slots (beyond @a numWorkers) for items.
     * @param outputGenerator    Output handler generator. The generator is passed a chunk
     *                           ID and @ref Timeplot::Worker, and returns a @ref Marching::OutputFunctor which
     *                           which will receive the output blocks for the corresponding chunk.
     * @param context            OpenCL context to run on.
     * @param device             OpenCL device to run on.
     * @param maxBucketSplats    Space to allocate for holding splats for one bucket.
     * @param maxCells           Space to allocate for the octree.
     * @param meshMemory         Maximum device bytes to use for mesh-related data.
     * @param levels             Levels to allocate for the octree.
     * @param subsampling        Octree subsampling level.
     * @param boundaryLimit      Tuning factor for boundary pruning.
     * @param shape              The shape to fit to the data
     */
    DeviceWorkerGroup(
        std::size_t numWorkers, std::size_t spare,
        OutputGenerator outputGenerator,
        const cl::Context &context, const cl::Device &device,
        std::size_t maxBucketSplats, Grid::size_type maxCells,
        std::size_t meshMemory,
        int levels, int subsampling, float boundaryLimit,
        MlsShape shape);

    /// Returns total resources that would be used by all workers and workitems
    static CLH::ResourceUsage resourceUsage(
        std::size_t numWorkers, std::size_t spare,
        const cl::Device &device,
        std::size_t maxBucketSplats, Grid::size_type maxCells,
        std::size_t meshMemory,
        int levels);

    /**
     * @copydoc WorkerGroup::start
     *
     * @param fullGrid  The bounding box grid.
     */
    void start(const Grid &fullGrid);

    /**
     * Sets a progress display that will be updated by the number of cells
     * processed.
     */
    void setProgress(ProgressMeter *progress) { this->progress = progress; }

    /**
     * Set a condition variable that will be signaled when space becomes
     * available in the item pool. The condition will be signaled with
     * the mutex held.
     */
    void setPopCondition(boost::mutex *mutex, boost::condition_variable *condition)
    {
        popMutex = mutex;
        popCondition = condition;
    }

    /**
     * @copydoc WorkerGroup::get
     */
    boost::shared_ptr<WorkItem> get(Timeplot::Worker &tworker, std::size_t size);

    /**
     * Determine whether @ref get will block.
     */
    bool canGet();

    /**
     * Returns the item to the pool. It is called by the base class.
     */
    void freeItem(boost::shared_ptr<WorkItem> item);

    /**
     * Estimate spare queue capacity. It takes the theoretical maximum capacity
     * and subtracts splats that are in the queue. It is not necessarily possible
     * to allocate this many.
     */
    std::size_t unallocated();

    /// Return the maximum number of splats that can be copied to a work item
    std::size_t getMaxItemSplats() const { return maxBucketSplats; }
    const cl::Context &getContext() const { return context; }
    const cl::Device &getDevice() const { return device; }
    const cl::CommandQueue &getCopyQueue() const { return copyQueue; }
    Statistics::Variable &getGetStat() const { return getStat; }
};

class CopyGroup;

class CopyGroupBase
{
public:
    /// A single bin of splats
    struct WorkItem
    {
        ChunkId chunkId;
        Grid grid;
        CircularBuffer::Allocation splats;  ///< Allocation from @ref CopyGroup::splatBuffer
        std::size_t numSplats;              ///< Number of splats in the bin

        Splat *getSplats() const { return (Splat *) splats.get(); }
    };

    class Worker : public WorkerBase
    {
    private:
        CopyGroup &owner;
        CLH::PinnedMemory<Splat> pinned;  ///< Staging area for copies
        /**
         * Bins that have been saved up but not yet flushed to the device.
         */
        Statistics::Container::vector<DeviceWorkerGroup::SubItem> bufferedItems;
        std::size_t bufferedSplats;       ///< Number of splats stored in @ref pinned

    public:
        typedef void result_type;

        Worker(CopyGroup &owner, const cl::Context &context, const cl::Device &device);

        void flush();   ///< Flush items in @ref bufferedItems to the output
        void operator()(WorkItem &work);
        void stop() { flush(); }
    };
};

/**
 * A worker object that copies bins of data to the GPU. It receives data from
 * @ref BucketLoader and sends it to the next available @ref DeviceWorkerGroup.
 */
class CopyGroup :
    protected CopyGroupBase,
    public WorkerGroup<CopyGroupBase::WorkItem, CopyGroupBase::Worker, CopyGroup>
{
public:
    typedef WorkerGroup<CopyGroupBase::WorkItem, CopyGroupBase::Worker, CopyGroup> BaseType;
    typedef CopyGroupBase::WorkItem WorkItem;

    /**
     * Constructor.
     * @param outGroups       Target devices. The first is used for allocating pinned memory.
     * @param maxQueueSplats  Splats to store in the internal queue.
     */
    CopyGroup(
        const std::vector<DeviceWorkerGroup *> &outGroups,
        std::size_t maxQueueSplats);

    /**
     * @copydoc WorkerGroup::get
     */
    boost::shared_ptr<WorkItem> get(Timeplot::Worker &tworker, std::size_t size)
    {
        boost::shared_ptr<WorkItem> item = BaseType::get(tworker, size);
        item->splats = splatBuffer.allocate(tworker, size * sizeof(Splat), &getStat);
        item->numSplats = size;
        return item;
    }

    /// Statistic for timing @c clEnqueueWriteBuffer
    Statistics::Variable &getWriteStat() const { return writeStat; }

private:
    const std::vector<DeviceWorkerGroup *> outGroups;
    const std::size_t maxDeviceItemSplats;     ///< Maximum splats to send to the device in one go
    CircularBuffer splatBuffer;                ///< Buffer holding incoming splats

    boost::mutex popMutex;                     ///< Mutex held while checking for device to target
    boost::condition_variable popCondition;    ///< Condition signalled by devices when space available

    Statistics::Variable &writeStat;           ///< See @ref getWriteStat
    Statistics::Variable &splatsStat;          ///< Number of splats per bin
    Statistics::Variable &sizeStat;            ///< Size of bins

    friend class CopyGroupBase::Worker;
};


/**
 * Wraps a worker group class to provide the @ref DeviceWorkerGroup::OutputGenerator
 * interface. The returned functor will push the data to the output group.
 */
template<typename OutGroup>
class OutputGeneratorBuilder
{
private:
    OutGroup &outGroup;

    /**
     * Provides @ref Marching::OutputFunctor interface.
     */
    class Functor
    {
    private:
        OutGroup &outGroup;
        ChunkId chunkId;
        Timeplot::Worker &tworker;
    public:
        typedef void result_type;
        Functor(OutGroup &outGroup, const ChunkId &chunkId, Timeplot::Worker &tworker)
            : outGroup(outGroup), chunkId(chunkId), tworker(tworker)
        {
        }

        void operator()(
            const cl::CommandQueue &queue,
            const DeviceKeyMesh &mesh,
            const std::vector<cl::Event> *events,
            cl::Event *event) const;
    };

public:
    typedef Marching::OutputFunctor result_type;

    explicit OutputGeneratorBuilder(OutGroup &outGroup)
        : outGroup(outGroup)
    {
    }

    result_type operator()(const ChunkId &chunkId, Timeplot::Worker &tworker) const
    {
        return Functor(outGroup, chunkId, tworker);
    }
};

template<typename OutGroup>
void OutputGeneratorBuilder<OutGroup>::Functor::operator()(
            const cl::CommandQueue &queue,
            const DeviceKeyMesh &mesh,
            const std::vector<cl::Event> *events,
            cl::Event *event) const
{
    std::size_t bytes = mesh.getHostBytes();

    boost::shared_ptr<typename OutGroup::WorkItem> item = outGroup.get(tworker, bytes);
    item->work.mesh = HostKeyMesh(item->alloc.get(), mesh);
    std::vector<cl::Event> wait(3);
    enqueueReadMesh(queue, mesh, item->work.mesh, events, &wait[0], &wait[1], &wait[2]);
    CLH::enqueueMarkerWithWaitList(queue, &wait, event);

    item->work.chunkId = chunkId;
    item->work.hasEvents = true;
    item->work.verticesEvent = wait[0];
    item->work.vertexKeysEvent = wait[1];
    item->work.trianglesEvent = wait[2];
    outGroup.push(tworker, item);
}

template<typename T>
DeviceWorkerGroup::OutputGenerator makeOutputGenerator(T &outGroup)
{
    return OutputGeneratorBuilder<T>(outGroup);
}

#endif /* !WORKERS_H */
