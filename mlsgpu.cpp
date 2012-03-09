/**
 * @file
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <boost/program_options.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/thread/thread.hpp>
#include <boost/array.hpp>
#include <boost/progress.hpp>
#include <boost/io/ios_state.hpp>
#include <tr1/unordered_map>
#include <iostream>
#include <map>
#include <vector>
#include <algorithm>
#include <stxxl.h>
#include "src/misc.h"
#include "src/clh.h"
#include "src/logging.h"
#include "src/timer.h"
#include "src/fast_ply.h"
#include "src/splat.h"
#include "src/grid.h"
#include "src/splat_tree_cl.h"
#include "src/marching.h"
#include "src/mls.h"
#include "src/mesher.h"
#include "src/options.h"
#include "src/splat_set.h"
#include "src/bucket.h"
#include "src/provenance.h"
#include "src/statistics.h"
#include "src/work_queue.h"
#include "src/progress.h"
#include "src/clip.h"
#include "src/mesh_filter.h"

namespace po = boost::program_options;
using namespace std;

namespace Option
{
    const char * const help = "help";
    const char * const quiet = "quiet";
    const char * const debug = "debug";
    const char * const responseFile = "response-file";

    const char * const fitSmooth = "fit-smooth";
    const char * const fitGrid = "fit-grid";
    const char * const fitPrune = "fit-prune";
    const char * const fitKeepBoundary = "fit-keep-boundary";
    const char * const fitBoundaryLimit = "fit-boundary-limit";

    const char * const inputFile = "input-file";
    const char * const outputFile = "output-file";

    const char * const statistics = "statistics";
    const char * const statisticsFile = "statistics-file";

    const char * const maxHostSplats = "max-host-splats";
    const char * const maxDeviceSplats = "max-device-splats";
    const char * const maxSplit = "max-split";
    const char * const levels = "levels";
    const char * const subsampling = "subsampling";
    const char * const bucketThreads = "bucket-threads";
    const char * const deviceThreads = "device-threads";
    const char * const mesher = "mesher";
    const char * const writer = "writer";
};

static void addCommonOptions(po::options_description &opts)
{
    opts.add_options()
        ("help,h",                "Show help")
        ("quiet,q",               "Do not show informational messages")
        (Option::debug,           "Show debug messages")
        (Option::responseFile,    "Read options from file");
}

static void addFitOptions(po::options_description &opts)
{
    opts.add_options()
        (Option::fitSmooth,       po::value<double>()->default_value(4.0),  "Smoothing factor")
        (Option::fitGrid,         po::value<double>()->default_value(0.01), "Spacing of grid cells")
        (Option::fitPrune,        po::value<double>()->default_value(0.02), "Minimum fraction of vertices per component")
        (Option::fitKeepBoundary,                                           "Do not remove boundaries")
        (Option::fitBoundaryLimit, po::value<double>()->default_value(1.5), "Tuning factor for boundary detection");
}

static void addStatisticsOptions(po::options_description &opts)
{
    po::options_description statistics("Statistics options");
    statistics.add_options()
        (Option::statistics,                          "Print information about internal statistics")
        (Option::statisticsFile, po::value<string>(), "Direct statistics to file instead of stdout (implies --statistics)");
    opts.add(statistics);
}

static void addAdvancedOptions(po::options_description &opts)
{
    po::options_description advanced("Advanced options");
    advanced.add_options()
        (Option::levels,       po::value<int>()->default_value(7), "Levels in octree")
        (Option::subsampling,  po::value<int>()->default_value(2), "Subsampling of octree")
        (Option::maxDeviceSplats, po::value<int>()->default_value(1000000), "Maximum splats per block on the device")
        (Option::maxHostSplats, po::value<std::size_t>()->default_value(50000000), "Maximum splats per block on the CPU")
        (Option::maxSplit,     po::value<int>()->default_value(2097152), "Maximum fan-out in partitioning")
        (Option::bucketThreads, po::value<int>()->default_value(4), "Number of threads for bucketing splats")
        (Option::deviceThreads, po::value<int>()->default_value(1), "Number of threads for submitting OpenCL work")
        (Option::mesher,       po::value<Choice<MesherTypeWrapper> >()->default_value(STXXL_MESHER), "Mesher (simple | weld | big | stxxl)")
        (Option::writer,       po::value<Choice<FastPly::WriterTypeWrapper> >()->default_value(FastPly::STREAM_WRITER), "File writer class (mmap | stream)");
    opts.add(advanced);
}

string makeOptions(const po::variables_map &vm)
{
    ostringstream opts;
    for (po::variables_map::const_iterator i = vm.begin(); i != vm.end(); ++i)
    {
        if (i->first == Option::inputFile)
            continue; // these are not output because some programs choke
        if (i->first == Option::responseFile)
            continue; // this is not relevant to reproducing the results
        const po::variable_value &param = i->second;
        const boost::any &value = param.value();
        if (param.empty()
            || (value.type() == typeid(string) && param.as<string>().empty()))
            opts << " --" << i->first;
        else if (value.type() == typeid(vector<string>))
        {
            BOOST_FOREACH(const string &j, param.as<vector<string> >())
            {
                opts << " --" << i->first << '=' << j;
            }
        }
        else
        {
            opts << " --" << i->first << '=';
            if (value.type() == typeid(string))
                opts << param.as<string>();
            else if (value.type() == typeid(double))
                opts << param.as<double>();
            else if (value.type() == typeid(int))
                opts << param.as<int>();
            else if (value.type() == typeid(std::size_t))
                opts << param.as<std::size_t>();
            else if (value.type() == typeid(Choice<MesherTypeWrapper>))
                opts << param.as<Choice<MesherTypeWrapper> >();
            else if (value.type() == typeid(Choice<FastPly::WriterTypeWrapper>))
                opts << param.as<Choice<FastPly::WriterTypeWrapper> >();
            else
                assert(!"Unhandled parameter type");
        }
    }
    return opts.str();
}

static void makeInputComments(FastPly::WriterBase *writer, const po::variables_map &vm)
{
    BOOST_FOREACH(const string &j, vm[Option::inputFile].as<vector<string> >())
    {
        writer->addComment("mlsgpu input: " + j);
    }
}

void writeStatistics(const boost::program_options::variables_map &vm, bool force = false)
{
    if (force || vm.count(Option::statistics) || vm.count(Option::statisticsFile))
    {
        ostream *out;
        ofstream outf;
        if (vm.count(Option::statisticsFile))
        {
            const string &name = vm[Option::statisticsFile].as<string>();
            outf.open(name.c_str());
            out = &outf;
        }
        else
        {
            out = &std::cout;
        }

        boost::io::ios_exception_saver saver(*out);
        out->exceptions(ios::failbit | ios::badbit);
        *out << Statistics::Registry::getInstance();
        *out << *stxxl::stats::get_instance();
    }
}

static void usage(ostream &o, const po::options_description desc)
{
    o << "Usage: mlsgpu [options] -o output.ply input.ply [input.ply...]\n\n";
    o << desc;
}

static po::variables_map processOptions(int argc, char **argv)
{
    po::positional_options_description positional;
    positional.add(Option::inputFile, -1);

    po::options_description desc("General options");
    addCommonOptions(desc);
    addFitOptions(desc);
    addStatisticsOptions(desc);
    addAdvancedOptions(desc);
    desc.add_options()
        ("output-file,o",   po::value<string>()->required(), "output file");

    po::options_description clopts("OpenCL options");
    CLH::addOptions(clopts);
    desc.add(clopts);

    po::options_description hidden("Hidden options");
    hidden.add_options()
        (Option::inputFile, po::value<vector<string> >()->composing(), "input files");

    po::options_description all("All options");
    all.add(desc);
    all.add(hidden);

    try
    {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv)
                  .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
                  .options(all)
                  .positional(positional)
                  .run(), vm);
        if (vm.count(Option::responseFile))
        {
            const string &fname = vm[Option::responseFile].as<string>();
            ifstream in(fname.c_str());
            if (!in)
            {
                Log::log[Log::warn] << "Could not open `" << fname << "', ignoring\n";
            }
            else
            {
                vector<string> args;
                copy(istream_iterator<string>(in), istream_iterator<string>(), back_inserter(args));
                if (in.bad())
                {
                    Log::log[Log::warn] << "Error while reading from `" << fname << "'\n";
                }
                in.close();
                po::store(po::command_line_parser(args)
                          .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
                          .options(all)
                          .positional(positional)
                          .run(), vm);
            }
        }

        po::notify(vm);

        if (vm.count(Option::help))
        {
            usage(cout, desc);
            exit(0);
        }
        /* Using ->required() on the option gives an unhelpful message */
        if (!vm.count(Option::inputFile))
        {
            cerr << "At least one input file must be specified.\n\n";
            usage(cerr, desc);
            exit(1);
        }

        return vm;
    }
    catch (po::error &e)
    {
        cerr << e.what() << "\n\n";
        usage(cerr, desc);
        exit(1);
    }
}

static void prepareInputs(boost::ptr_vector<FastPly::Reader> &files, const po::variables_map &vm, float smooth)
{
    const vector<string> &names = vm[Option::inputFile].as<vector<string> >();
    files.clear();
    files.reserve(names.size());
    BOOST_FOREACH(const string &name, names)
    {
        FastPly::Reader *reader = new FastPly::Reader(name, smooth);
        files.push_back(reader);
    }
}

struct HostWorkItem
{
    std::vector<Splat> splats;
    Grid grid;
    Bucket::Recursion recursionState;
};

struct DeviceWorkItem
{
    std::vector<Splat> splats;
    Grid grid;
    Bucket::Recursion recursionState;
};

/**
 * Does the actual OpenCL calls necessary to compute the mesh and write
 * it to the @ref MesherBase class. It pulls chunks of work off a queue,
 * which contains pre-bucketed splats.
 *
 * It is intended to be used as a function object for @c boost::thread.
 */
class DeviceWorker : public boost::noncopyable
{
private:
    WorkQueue<boost::shared_ptr<DeviceWorkItem> > &workQueue;

    const Grid &fullGrid;

    const cl::CommandQueue queue;
    SplatTreeCL tree;
    MlsFunctor input;
    Marching marching;
    boost::scoped_ptr<Clip> clip;
    ScaleBiasFilter scaleBias;
    Marching::OutputFunctor output;
    MeshFilterChain filterChain;

    std::size_t maxSplats;
    Grid::size_type maxCells;
    int subsampling;

    ProgressDisplay *progress;

public:
    typedef void result_type;
    typedef StdVectorCollection<Splat> Collection;

    DeviceWorker(
        WorkQueue<boost::shared_ptr<DeviceWorkItem> > &workQueue,
        const Grid &fullGrid,
        const cl::Context &context, const cl::Device &device,
        std::size_t maxSplats, Grid::size_type maxCells,
        int levels, int subsampling, bool keepBoundary, float boundaryLimit);

    static CLH::ResourceUsage resourceUsage(
        const cl::Device &device,
        std::size_t maxSplats, Grid::size_type maxCells,
        int levels, bool keepBoundary);

    /// Thread function.
    void operator()();

    void setProgress(ProgressDisplay *progress) { this->progress = progress; }

    void setOutput(const Marching::OutputFunctor &output)
    {
        filterChain.setOutput(output);
    }
};

DeviceWorker::DeviceWorker(
    WorkQueue<boost::shared_ptr<DeviceWorkItem> > &workQueue,
    const Grid &fullGrid,
    const cl::Context &context, const cl::Device &device,
    std::size_t maxSplats, Grid::size_type maxCells,
    int levels, int subsampling, bool keepBoundary, float boundaryLimit)
:
    workQueue(workQueue),
    fullGrid(fullGrid),
    queue(context, device),
    tree(context, levels, maxSplats),
    input(context),
    marching(context, device, maxCells + 1, maxCells + 1),
    scaleBias(context),
    maxSplats(maxSplats), maxCells(maxCells),
    subsampling(subsampling),
    progress(NULL)
{
    if (!keepBoundary)
    {
        input.setBoundaryLimit(boundaryLimit);
        clip.reset(new Clip(context, device,
                            marching.getMaxVertices(maxCells + 1, maxCells + 1),
                            marching.getMaxTriangles(maxCells + 1, maxCells + 1)));
        clip->setDistanceFunctor(input);
        filterChain.addFilter(boost::ref(*clip));
    }

    filterChain.addFilter(boost::ref(scaleBias));
}

CLH::ResourceUsage DeviceWorker::resourceUsage(
    const cl::Device &device,
    std::size_t maxSplats, Grid::size_type maxCells,
    int levels, bool keepBoundary)
{
    Grid::size_type block = maxCells + 1;
    std::size_t maxVertices = Marching::getMaxVertices(block, block);
    std::size_t maxTriangles = Marching::getMaxTriangles(block, block);
    CLH::ResourceUsage marchingUsage = Marching::resourceUsage(device, block, block);
    CLH::ResourceUsage splatTreeUsage = SplatTreeCL::resourceUsage(device, levels, maxSplats);
    CLH::ResourceUsage clipUsage;
    if (!keepBoundary)
        clipUsage = Clip::resourceUsage(device, maxVertices, maxTriangles);
    return marchingUsage + splatTreeUsage + clipUsage;
}

void DeviceWorker::operator()()
{
    scaleBias.setScaleBias(fullGrid);

    while (true)
    {
        boost::shared_ptr<DeviceWorkItem> item;
        {
            Statistics::Timer timer("device.worker.pop");
            item = workQueue.pop();
        }
        if (!item)
            break;

        cl_uint3 keyOffset; 
        for (int i = 0; i < 3; i++)
            keyOffset.s[i] = item->grid.getExtent(i).first;
        // same thing, just as a different type for a different API
        Grid::difference_type offset[3] = { keyOffset.s[0], keyOffset.s[1], keyOffset.s[2] };

        Grid::size_type size[3];
        for (int i = 0; i < 3; i++)
        {
            /* Note: numVertices not numCells, because Marching does per-vertex queries.
             * So we need information about the cell that is just beyond the last vertex,
             * just to avoid special-casing it.
             */
            size[i] = item->grid.numVertices(i);
        }

        /* We need to round up the octree size to a multiple of the granularity used for MLS. */
        Grid::size_type expandedSize[3];
        for (int i = 0; i < 2; i++)
            expandedSize[i] = roundUp(size[i], MlsFunctor::wgs[i]);
        expandedSize[2] = size[2];

        // TODO: use mapping to transfer the data directly into a buffer

        {
            Statistics::Timer timer("device.worker.time");
            cl::Event treeBuildEvent;
            vector<cl::Event> wait(1);
            tree.enqueueBuild(queue, &item->splats[0], item->splats.size(),
                              expandedSize, offset, subsampling, CL_FALSE, NULL, &treeBuildEvent);
            wait[0] = treeBuildEvent;

            input.set(expandedSize, offset, tree, subsampling);
            marching.generate(queue, input, filterChain, size, keyOffset, &wait);
        }

        if (progress != NULL)
            *progress += item->grid.numCells();
    }
}

/**
 * A thread function object that handles coarse-to-fine bucketing. It pulls
 * work from one queue (containing regions of splats already read from storage),
 * calls @ref Bucket::bucket to subdivide the splats into buckets suitable for
 * device execution, and passes them on to another queue.
 */
class DeviceBlock
{
public:
    typedef void result_type;
    typedef SplatSet::SimpleSet<boost::ptr_vector<StdVectorCollection<Splat> > > Set;

    /// Bucketing callback for blocks sized for device execution.
    void operator()(
        const Set &splatSet,
        Bucket::Range::index_type numSplats,
        Bucket::RangeConstIterator first,
        Bucket::RangeConstIterator last,
        const Grid &grid,
        const Bucket::Recursion &recursionState);

    /// Thread runner
    void operator()();

    void setProgress(ProgressDisplay *progress) { this->progress = progress; }

    DeviceBlock(WorkQueue<boost::shared_ptr<HostWorkItem> > &workQueueIn,
                WorkQueue<boost::shared_ptr<DeviceWorkItem> > &workQueueOut,
                const Grid &fullGrid,
                std::size_t maxSplats,
                Grid::size_type maxCells,
                std::size_t maxSplit);
private:
    WorkQueue<boost::shared_ptr<HostWorkItem> > &workQueueIn;
    WorkQueue<boost::shared_ptr<DeviceWorkItem> > &workQueueOut;

    const Grid &fullGrid;
    std::size_t maxSplats;
    Grid::size_type maxCells;
    std::size_t maxSplit;
    ProgressDisplay *progress;
};

DeviceBlock::DeviceBlock(
    WorkQueue<boost::shared_ptr<HostWorkItem> > &workQueueIn,
    WorkQueue<boost::shared_ptr<DeviceWorkItem> > &workQueueOut,
    const Grid &fullGrid,
    std::size_t maxSplats,
    Grid::size_type maxCells,
    std::size_t maxSplit)
:
    workQueueIn(workQueueIn),
    workQueueOut(workQueueOut),
    fullGrid(fullGrid),
    maxSplats(maxSplats),
    maxCells(maxCells),
    maxSplit(maxSplit),
    progress(NULL)
{
}

void DeviceBlock::operator()(
    const Set &splatSet,
    Bucket::Range::index_type numSplats,
    Bucket::RangeConstIterator first,
    Bucket::RangeConstIterator last,
    const Grid &grid,
    const Bucket::Recursion &recursionState)
{
    Statistics::Registry &registry = Statistics::Registry::getInstance();

    vector<Splat> outSplats(numSplats);

    std::size_t pos = 0;
    // Stats bookkeeping
    // TODO: reuse code from HostBlock
    const int pageSize = 4096;
    std::size_t numPages = 0;
    std::size_t lastPage = (std::size_t) -1;
    for (Bucket::RangeConstIterator i = first; i != last; i++)
    {
        assert(pos + i->size <= numSplats);
        Bucket::Range::scan_type scan = i->scan;
        // Note: &outSplats[pos] is necessary to trigger the fast path in
        // FastPly::Reader. Using outSplats.begin() + pos would hit the
        // slow path.
        splatSet.getSplats()[scan].read(i->start, i->start + i->size, &outSplats[pos]);
        pos += i->size;

        if (i->size > 0)
        {
            std::size_t pageFirst = i->start / pageSize;
            std::size_t pageLast = (i->start + i->size - 1) / pageSize;
            numPages += pageLast - pageFirst + 1;
            if (lastPage == pageFirst)
                numPages--;
            lastPage = pageLast;
        }
    }
    assert(pos == numSplats);

    registry.getStatistic<Statistics::Variable>("device.block.splats").add(numSplats);
    registry.getStatistic<Statistics::Variable>("device.block.ranges").add(last - first);
    registry.getStatistic<Statistics::Variable>("device.block.pagedSplats").add(numPages * pageSize);
    registry.getStatistic<Statistics::Variable>("device.block.size").add(grid.numCells());

    boost::shared_ptr<DeviceWorkItem> item = boost::make_shared<DeviceWorkItem>();
    item->splats.swap(outSplats);
    item->grid = grid;
    item->recursionState = recursionState;

    {
        Statistics::Timer timer("device.block.push");
        workQueueOut.push(item);
    }
}

void DeviceBlock::operator()()
{
    while (true)
    {
        boost::shared_ptr<HostWorkItem> item;
        {
            Statistics::Timer timer("device.block.pop");
            item = workQueueIn.pop();
        }
        if (!item)
            break;

        Statistics::Timer timer("device.block.exec");
        boost::ptr_vector<StdVectorCollection<Splat> > deviceSplats;
        SplatSet::SimpleSet<boost::ptr_vector<StdVectorCollection<Splat> > > splatSet(deviceSplats);
        deviceSplats.push_back(new StdVectorCollection<Splat>(item->splats));

        /* The host transformed splats from world space into fullGrid space, so we need to
         * construct a new grid for this coordinate system.
         */
        const float ref[3] = {0.0f, 0.0f, 0.0f};
        Grid grid(ref, 1.0f, 0, 1, 0, 1, 0, 1);
        for (unsigned int i = 0; i < 3; i++)
        {
            Grid::difference_type base = fullGrid.getExtent(i).first;
            Grid::difference_type low = item->grid.getExtent(i).first - base;
            Grid::difference_type high = item->grid.getExtent(i).second - base;
            grid.setExtent(i, low, high);
        }
        Bucket::bucket(splatSet, grid, maxSplats, maxCells, false, maxSplit,
                       boost::ref(*this), progress, item->recursionState);
    }
}

/**
 * Handles coarse-level bucketing from external storage. Unlike @ref
 * DeviceWorker and @ref DeviceBlock, there is only expected to be one of
 * these, and it does not run in a separate thread. It produces coarse
 * buckets, read the splats into memory and pushes the results to a queue.
 */
template<typename Set>
class HostBlock
{
public:
    void operator()(
        const Set &splatSet,
        Bucket::Range::index_type numSplats,
        Bucket::RangeConstIterator first,
        Bucket::RangeConstIterator last,
        const Grid &grid,
        const Bucket::Recursion &recursionState) const;

    HostBlock(WorkQueue<boost::shared_ptr<HostWorkItem> > &workQueue,
              const Grid &fullGrid);
private:
    WorkQueue<boost::shared_ptr<HostWorkItem> > &workQueue;
    const Grid &fullGrid;
};

template<typename Collection>
HostBlock<Collection>::HostBlock(WorkQueue<boost::shared_ptr<HostWorkItem> > &workQueue,
                                 const Grid &fullGrid)
: workQueue(workQueue), fullGrid(fullGrid)
{
}

template<typename Set>
void HostBlock<Set>::operator()(
    const Set &splatSet,
    Bucket::Range::index_type numSplats,
    Bucket::RangeConstIterator first, Bucket::RangeConstIterator last,
    const Grid &grid, const Bucket::Recursion &recursionState) const
{
    Statistics::Registry &registry = Statistics::Registry::getInstance();

    boost::shared_ptr<HostWorkItem> item = boost::make_shared<HostWorkItem>();
    item->grid = grid;
    item->recursionState = recursionState;
    float invSpacing = 1.0f / fullGrid.getSpacing();

    {
        Statistics::Timer timer("host.block.load");
        std::size_t pos = 0;
        item->splats.resize(numSplats);

        // Stats bookkeeping
        const int pageSize = 4096;
        std::size_t numPages = 0;
        std::size_t lastPage = (std::size_t) -1;
        for (Bucket::RangeConstIterator i = first; i != last; i++)
        {
            assert(pos + i->size <= numSplats);
            Bucket::Range::scan_type scan = i->scan;
            // Note: &localSplats[pos] is necessary to trigger the fast path in
            // FastPly::Reader. Using outSplats.begin() + pos would hit the
            // slow path.
            splatSet.getSplats()[scan].read(i->start, i->start + i->size, &item->splats[pos]);
            /* Transform the splats into the grid's coordinate system */
            for (size_t j = pos; j < pos + i->size; j++)
            {
                fullGrid.worldToVertex(item->splats[j].position, item->splats[j].position);
                item->splats[j].radius *= invSpacing;
            }
            pos += i->size;

            if (i->size > 0)
            {
                std::size_t pageFirst = i->start / pageSize;
                std::size_t pageLast = (i->start + i->size - 1) / pageSize;
                numPages += pageLast - pageFirst + 1;
                if (lastPage == pageFirst)
                    numPages--;
                lastPage = pageLast;
            }
        }
        assert(pos == numSplats);

        registry.getStatistic<Statistics::Variable>("host.block.splats").add(numSplats);
        registry.getStatistic<Statistics::Variable>("host.block.ranges").add(last - first);
        registry.getStatistic<Statistics::Variable>("host.block.pagedSplats").add(numPages * pageSize);
        registry.getStatistic<Statistics::Variable>("host.block.size").add
            (double(grid.numCells(0)) * grid.numCells(1) * grid.numCells(2));
    }

    {
        Statistics::Timer timer("host.block.push");
        workQueue.push(item);
    }
}

/**
 * Second phase of execution, which is templated on the collection type
 * (which in turn depends on whether --sort was given or not).
 *
 * @todo --sort is gone for now.
 */
template<typename Set>
static void run2(const cl::Context &context, const cl::Device &device, const string &out,
                 const po::variables_map &vm,
                 const Set &splatSet,
                 const Grid &grid)
{
    const int subsampling = vm[Option::subsampling].as<int>();
    const int levels = vm[Option::levels].as<int>();
    const FastPly::WriterType writerType = vm[Option::writer].as<Choice<FastPly::WriterTypeWrapper> >();
    const MesherType mesherType = vm[Option::mesher].as<Choice<MesherTypeWrapper> >();
    const std::size_t maxDeviceSplats = vm[Option::maxDeviceSplats].as<int>();
    const std::size_t maxHostSplats = vm[Option::maxHostSplats].as<std::size_t>();
    const std::size_t maxSplit = vm[Option::maxSplit].as<int>();
    const double pruneThreshold = vm[Option::fitPrune].as<double>();
    const bool keepBoundary = vm.count(Option::fitKeepBoundary);
    const float boundaryLimit = vm[Option::fitBoundaryLimit].as<double>();

    const unsigned int block = 1U << (levels + subsampling - 1);
    const unsigned int blockCells = block - 1;

    /*
     * TODO:
     * - support multithreading at the next level down as well, to
     *   do better host-device overlap
     */
    const unsigned int numBucketThreads = vm[Option::bucketThreads].as<int>();
    const unsigned int numDeviceThreads = vm[Option::deviceThreads].as<int>();

    WorkQueue<boost::shared_ptr<HostWorkItem> > workQueueCoarse(1);
    WorkQueue<boost::shared_ptr<DeviceWorkItem> > workQueueFine(2);

    boost::ptr_vector<DeviceBlock> deviceBlocks;
    boost::ptr_vector<DeviceWorker> deviceWorkers;
    deviceBlocks.reserve(numBucketThreads);
    deviceWorkers.reserve(numDeviceThreads);
    for (unsigned int i = 0; i < numBucketThreads; i++)
    {
        Statistics::Timer timer("device.block.init");
        deviceBlocks.push_back(new DeviceBlock(
                workQueueCoarse, workQueueFine, grid,
                maxDeviceSplats, blockCells, maxSplit));
    }
    for (unsigned int i = 0; i < numDeviceThreads; i++)
    {
        Statistics::Timer timer("device.worker.init");
        deviceWorkers.push_back(new DeviceWorker(
                workQueueFine, grid,
                context, device,
                maxDeviceSplats, blockCells,
                levels, subsampling,
                keepBoundary, boundaryLimit));
    }
    HostBlock<Set> hostBlock(workQueueCoarse, grid);

    boost::scoped_ptr<FastPly::WriterBase> writer(FastPly::createWriter(writerType));
    writer->addComment("mlsgpu version: " + provenanceVersion());
    writer->addComment("mlsgpu variant: " + provenanceVariant());
    writer->addComment("mlsgpu options:" + makeOptions(vm));
    makeInputComments(writer.get(), vm);
    boost::scoped_ptr<MesherBase> mesher(createMesher(mesherType, *writer, out));
    mesher->setPruneThreshold(pruneThreshold);
    for (unsigned int pass = 0; pass < mesher->numPasses(); pass++)
    {
        Log::log[Log::info] << "\nPass " << pass + 1 << "/" << mesher->numPasses() << endl;
        ostringstream passName;
        passName << "pass" << pass + 1 << ".time";
        Statistics::Timer timer(passName.str());

        ProgressDisplay progress(grid.numCells(), Log::log[Log::info]);
        Marching::OutputFunctor out = mesher->outputFunctor(pass);

        // Start threads
        boost::thread_group bucketThreads;
        boost::thread_group workerThreads;
        for (unsigned int i = 0; i < numBucketThreads; i++)
        {
            deviceBlocks[i].setProgress(&progress);
            bucketThreads.create_thread(boost::bind(boost::ref(deviceBlocks[i])));
        }
        for (unsigned int i = 0; i < numDeviceThreads; i++)
        {
            deviceWorkers[i].setOutput(out);
            deviceWorkers[i].setProgress(&progress);
            workerThreads.create_thread(boost::bind(boost::ref(deviceWorkers[i])));
        }

        Bucket::bucket(splatSet, grid, maxHostSplats, blockCells, true, maxSplit, hostBlock, &progress);

        /* Shut down bucket threads. Note that these have to be completely shut
         * down before we start shutting down the worker threads, as otherwise
         * we might kill the worker threads before all their work has been
         * queued to them.
         */
        for (unsigned int i = 0; i < numBucketThreads; i++)
            workQueueCoarse.push(boost::shared_ptr<HostWorkItem>());
        bucketThreads.join_all();

        // Now kill the worker threads
        for (unsigned int i = 0; i < numDeviceThreads; i++)
            workQueueFine.push(boost::shared_ptr<DeviceWorkItem>());
        workerThreads.join_all();

        assert(workQueueCoarse.size() == 0);
        assert(workQueueFine.size() == 0);
    }


    {
        Statistics::Timer timer("finalize.time");

        mesher->finalize(&Log::log[Log::info]);
        mesher->write(*writer, out, &Log::log[Log::info]);
    }
}

static void run(const cl::Context &context, const cl::Device &device, const string &out,
                const po::variables_map &vm)
{
    const float spacing = vm[Option::fitGrid].as<double>();
    const float smooth = vm[Option::fitSmooth].as<double>();
    const int subsampling = vm[Option::subsampling].as<int>();
    const int levels = vm[Option::levels].as<int>();
    const unsigned int block = 1U << (levels + subsampling - 1);
    const unsigned int blockCells = block - 1;

    boost::ptr_vector<FastPly::Reader> files;
    prepareInputs(files, vm, smooth);
    Grid grid;

    typedef stxxl::VECTOR_GENERATOR<SplatSet::Blob>::result BlobVector;
    typedef SplatSet::BlobSet<boost::ptr_vector<FastPly::Reader>, BlobVector> Set;
    boost::scoped_ptr<Set> splatSet;
    try
    {
        Statistics::Timer timer("bbox.time");
        splatSet.reset(new Set(files, spacing, blockCells, &Log::log[Log::info]));
    }
    catch (std::length_error &e)
    {
        cerr << "At least one input point is required.\n";
        exit(1);
    }

    run2(context, device, out, vm, *splatSet, splatSet->getBoundingGrid());
    writeStatistics(vm);
}

static void validateOptions(const cl::Device &device, const po::variables_map &vm)
{
    if (!Marching::validateDevice(device)
        || !SplatTreeCL::validateDevice(device))
    {
        cerr << "This OpenCL device is not supported.\n";
        exit(1);
    }

    const int levels = vm[Option::levels].as<int>();
    const int subsampling = vm[Option::subsampling].as<int>();
    const std::size_t maxDeviceSplats = vm[Option::maxDeviceSplats].as<int>();
    const std::size_t maxHostSplats = vm[Option::maxHostSplats].as<std::size_t>();
    const std::size_t maxSplit = vm[Option::maxSplit].as<int>();
    const int bucketThreads = vm[Option::bucketThreads].as<int>();
    const int deviceThreads = vm[Option::deviceThreads].as<int>();
    const double pruneThreshold = vm[Option::fitPrune].as<double>();
    const bool keepBoundary = vm.count(Option::fitKeepBoundary);

    int maxLevels = std::min(std::size_t(Marching::MAX_DIMENSION_LOG2 + 1), SplatTreeCL::MAX_LEVELS);
    /* TODO make dynamic, considering maximum image sizes etc */
    if (levels < 1 || levels > maxLevels)
    {
        cerr << "Value of --levels must be in the range 1 to " << maxLevels << ".\n";
        exit(1);
    }
    if (subsampling < 0)
    {
        cerr << "Value of --subsampling must be non-negative.\n";
        exit(1);
    }
    if (maxDeviceSplats < 1)
    {
        cerr << "Value of --max-device-splats must be positive.\n";
        exit(1);
    }
    if (maxHostSplats < maxDeviceSplats)
    {
        cerr << "Value of --max-host-splats must be at least that of --max-device-splats.\n";
        exit(1);
    }
    if (maxSplit < 8)
    {
        cerr << "Value of --max-split must be at least 8.\n";
        exit(1);
    }
    if (subsampling > Marching::MAX_DIMENSION_LOG2 + 1 - levels)
    {
        cerr << "Sum of --subsampling and --levels is too large.\n";
        exit(1);
    }
    const std::size_t treeVerts = std::size_t(1) << (subsampling + levels - 1);
    if (treeVerts < MlsFunctor::wgs[0] || treeVerts < MlsFunctor::wgs[1])
    {
        cerr << "Sum of --subsampling and --levels it too small.\n";
        exit(1);
    }

    if (bucketThreads < 1)
    {
        cerr << "Value of --bucket-threads must be at least 1\n";
        exit(1);
    }
    if (deviceThreads < 1)
    {
        cerr << "Value of --device-threads must be at least 1\n";
        exit(1);
    }
    if (!(pruneThreshold >= 0.0 && pruneThreshold <= 1.0))
    {
        cerr << "Value of --fit-prune must be in [0, 1]\n";
        exit(1);
    }

    /* Check that we have enough memory on the device. This is no guarantee against OOM, but
     * we can at least turn down silly requests before wasting any time.
     */
    const Grid::size_type maxCells = (Grid::size_type(1U) << (levels + subsampling - 1)) - 1;
    CLH::ResourceUsage threadUsage = DeviceWorker::resourceUsage(
        device, maxDeviceSplats, maxCells, levels, keepBoundary);
    CLH::ResourceUsage totalUsage = threadUsage * deviceThreads;

    const std::size_t deviceTotalMemory = device.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>();
    const std::size_t deviceMaxMemory = device.getInfo<CL_DEVICE_MAX_MEM_ALLOC_SIZE>();
    if (totalUsage.getMaxMemory() > deviceMaxMemory)
    {
        cerr << "Arguments require an allocation of " << totalUsage.getMaxMemory() << ",\n"
            << "but the OpenCL device only supports up to " << deviceMaxMemory << ".\n"
            << "Try reducing --levels or --subsampling.\n";
        exit(1);
    }
    if (totalUsage.getTotalMemory() > deviceTotalMemory)
    {
        cerr << "Arguments require device memory of " << totalUsage.getTotalMemory() << ",\n"
            << "but the OpenCL device has " << deviceTotalMemory << ".\n"
            << "Try reducing --levels or --subsampling.\n";
        exit(1);
    }

    Log::log[Log::info] << "About " << totalUsage.getTotalMemory() / (1024 * 1024) << "MiB of device memory will be used.\n";
    if (totalUsage.getTotalMemory() > deviceTotalMemory * 0.8)
    {
        Log::log[Log::warn] << "WARNING: More than 80% of the device memory will be used.\n";
    }
}

int main(int argc, char **argv)
{
    Log::log.setLevel(Log::debug);

    po::variables_map vm = processOptions(argc, argv);
    if (vm.count(Option::quiet))
        Log::log.setLevel(Log::warn);
    else if (vm.count(Option::debug))
        Log::log.setLevel(Log::debug);

    cl::Device device = CLH::findDevice(vm);
    if (!device())
    {
        cerr << "No suitable OpenCL device found\n";
        exit(1);
    }
    Log::log[Log::info] << "Using device " << device.getInfo<CL_DEVICE_NAME>() << "\n";

    validateOptions(device, vm);

    cl::Context context = CLH::makeContext(device);

    try
    {
        run(context, device, vm[Option::outputFile].as<string>(), vm);
    }
    catch (ios::failure &e)
    {
        cerr << e.what() << '\n';
        return 1;
    }
    catch (PLY::FormatError &e)
    {
        cerr << e.what() << '\n';
        return 1;
    }
    catch (cl::Error &e)
    {
        cerr << "OpenCL error in " << e.what() << " (" << e.err() << ")\n";
        return 1;
    }
    catch (Bucket::DensityError &e)
    {
        cerr << "The splats were too dense. Try passing a higher value for --max-device-splats.\n";
        return 1;
    }

    return 0;
}
