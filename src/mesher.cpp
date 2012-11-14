/**
 * @file
 *
 * Data structures for storing the output of @ref Marching.
 *
 * @todo Audit for overflows when >2^32 vertices emitted in total but chunked.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include "tr1_cstdint.h"
#include <CL/cl.h>
#include <vector>
#include <boost/array.hpp>
#include <boost/function.hpp>
#include <boost/bind/bind.hpp>
#include <boost/ref.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/foreach.hpp>
#include <boost/type_traits/make_unsigned.hpp>
#include <boost/exception/all.hpp>
#include "tr1_unordered_map.h"
#include <cassert>
#include <cstdlib>
#include <utility>
#include <iterator>
#include <map>
#include <string>
#include <ostream>
#include <iomanip>
#include <cerrno>
#include "mesher.h"
#include "fast_ply.h"
#include "logging.h"
#include "errors.h"
#include "progress.h"
#include "union_find.h"
#include "statistics.h"
#include "clh.h"

std::map<std::string, MesherType> MesherTypeWrapper::getNameMap()
{
    std::map<std::string, MesherType> ans;
    ans["stxxl"] = STXXL_MESHER;
    return ans;
}

std::string ChunkNamer::operator()(const ChunkId &chunkId) const
{
    std::ostringstream nameStream;
    nameStream << baseName;
    for (unsigned int i = 0; i < 3; i++)
        nameStream << '_' << std::setw(4) << std::setfill('0') << chunkId.coords[i];
    nameStream << ".ply";
    return nameStream.str();
}


StxxlMesher::VertexBuffer::VertexBuffer(FastPly::WriterBase &writer, size_type capacity)
    : writer(writer), nextVertex(0)
{
    buffer.reserve(capacity);
}

void StxxlMesher::VertexBuffer::operator()(const boost::array<float, 3> &vertex)
{
    buffer.push_back(vertex);
    if (buffer.size() == buffer.capacity())
        flush();
}

void StxxlMesher::VertexBuffer::flush()
{
    writer.writeVertices(nextVertex, buffer.size(), &buffer[0][0]);
    nextVertex += buffer.size();
    buffer.clear();
}

StxxlMesher::TriangleBuffer::TriangleBuffer(FastPly::WriterBase &writer, size_type capacity)
    : writer(writer), nextTriangle(0)
{
    nextTriangle = 0;
    buffer.reserve(capacity);
}

void StxxlMesher::TriangleBuffer::operator()(const boost::array<std::tr1::uint32_t, 3> &triangle)
{
    buffer.push_back(triangle);
    if (buffer.size() == buffer.capacity())
        flush();
}

void StxxlMesher::computeLocalComponents(
    std::size_t numVertices,
    const Statistics::Container::vector<boost::array<cl_uint, 3> > &triangles,
    Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > &nodes)
{
    nodes.clear();
    nodes.resize(numVertices);
    typedef boost::array<cl_uint, 3> triangle_type;
    BOOST_FOREACH(const triangle_type &triangle, triangles)
    {
        // Only need to use two edges in the union-find tree, since the
        // third will be redundant.
        for (unsigned int j = 0; j < 2; j++)
            UnionFind::merge(nodes, triangle[j], triangle[j + 1]);
    }
}

void StxxlMesher::updateGlobalClumps(
    const Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > &nodes,
    const Statistics::Container::vector<boost::array<cl_uint, 3> > &triangles,
    Statistics::Container::vector<clump_id> &clumpId)
{
    std::size_t numVertices = nodes.size();

    // Allocate clumps for the local components
    clumpId.resize(numVertices);
    for (std::size_t i = 0; i < numVertices; i++)
    {
        if (nodes[i].isRoot())
        {
            if (clumps.size() >= boost::make_unsigned<clump_id>::type(std::numeric_limits<clump_id>::max()))
            {
                /* Ideally this would throw, but it's called from a worker
                 * thread and there is no easy way to immediately notify the
                 * master thread that it should shut everything down.
                 */
                std::cerr << "There were too many connected components.\n";
                std::exit(1);
            }
            clumpId[i] = clumps.size();
            clumps.push_back(Clump(nodes[i].size()));
        }
    }

    // Compute clump IDs for the non-root vertices
    for (std::size_t i = 0; i < numVertices; i++)
    {
        std::tr1::int32_t r = UnionFind::findRoot(nodes, i);
        clumpId[i] = clumpId[r];
    }

    // Compute triangle counts for the clumps
    typedef boost::array<cl_uint, 3> triangle_type;
    BOOST_FOREACH(const triangle_type &triangle, triangles)
    {
        Clump &clump = clumps[clumpId[triangle[0]]];
        clump.triangles++;
    }
}

void StxxlMesher::updateClumpKeyMap(
    const Statistics::Container::vector<cl_ulong> &keys,
    const Statistics::Container::vector<clump_id> &clumpId)
{
    const std::size_t numExternalVertices = keys.size();
    const std::size_t numInternalVertices = clumpId.size() - numExternalVertices;

    for (std::size_t i = 0; i < numExternalVertices; i++)
    {
        cl_ulong key = keys[i];
        clump_id cid = clumpId[i + numInternalVertices];

        std::pair<Statistics::Container::unordered_map<cl_ulong, clump_id>::const_iterator, bool> added;
        added = clumpIdMap.insert(std::make_pair(key, cid));
        if (!added.second)
        {
            // Unified two external vertices. Also need to unify their clumps.
            clump_id cid2 = added.first->second;
            UnionFind::merge(clumps, cid, cid2);
            // They will both have counted the common vertex, so we need to
            // subtract it.
            cid = UnionFind::findRoot(clumps, cid);
            clumps[cid].vertices--;
        }
    }
}

void StxxlMesher::flushBuffer()
{
    Statistics::Timer flushTimer("mesher.flush");
    BOOST_FOREACH(Chunk &chunk, chunks)
    {
        if (!chunk.bufferedClumps.empty())
        {
            BOOST_FOREACH(const Chunk::Clump &clump, chunk.bufferedClumps)
            {
                vertices_type::size_type firstVertex = vertices.size();
                triangles_type::size_type firstTriangle = triangles.size();
                for (std::size_t i = 0; i < clump.numInternalVertices + clump.numExternalVertices; i++)
                    vertices.push_back(verticesBuffer[clump.firstVertex + i]);
                for (std::size_t i = 0; i < clump.numTriangles; i++)
                    triangles.push_back(trianglesBuffer[clump.firstTriangle + i]);
                chunk.clumps.push_back(Chunk::Clump(
                    firstVertex,
                    clump.numInternalVertices,
                    clump.numExternalVertices,
                    firstTriangle,
                    clump.numTriangles,
                    clump.globalId));
            }
            chunk.bufferedClumps.clear();
        }
    }
    verticesBuffer.clear();
    trianglesBuffer.clear();
}

void StxxlMesher::updateLocalClumps(
    Chunk &chunk,
    const Statistics::Container::vector<clump_id> &globalClumpId,
    HostKeyMesh &mesh)
{
    const std::size_t numVertices = mesh.vertices.size();
    const std::size_t numInternalVertices = numVertices - mesh.vertexKeys.size();

    tmpVertexLabel.clear();
    tmpVertexLabel.resize(numVertices);
    tmpVertexOrder.clear();

    // Vertices are not sorted in-place, but rather we sort vertex indices. This avoids
    // the need to either bundle vertex data together with original indices or to use
    // a custom sort for extracting the permutation.
    tmpVertexOrder.reserve(numVertices);
    for (std::size_t i = 0; i < numVertices; i++)
        tmpVertexOrder.push_back(i);
    std::sort(tmpVertexOrder.begin(), tmpVertexOrder.end(), VertexCompare(globalClumpId));
    std::sort(mesh.triangles.begin(), mesh.triangles.end(), TriangleCompare(globalClumpId));

    std::size_t nextVertex = 0;
    std::size_t nextTriangle = 0;
    if ((numVertices + verticesBuffer.size()) * sizeof(vertices_type::value_type)
        + (mesh.triangles.size() + trianglesBuffer.size()) * sizeof(triangles_type::value_type)
        > getReorderCapacity())
        flushBuffer();

    while (nextVertex < numVertices)
    {
        // This loop is run once per clump
        clump_id cid = globalClumpId[tmpVertexOrder[nextVertex]];
        std::size_t lastVertex = nextVertex;
        std::size_t lastTriangle = nextTriangle;
        // These count *emitted* vertices - which for external vertices can be less than
        // incoming ones due to sharing within the chunk.
        std::size_t clumpInternalVertices = 0;
        std::size_t clumpExternalVertices = 0;
        do
        {
            std::tr1::uint32_t vid = tmpVertexOrder[nextVertex];
            bool elide = false; // true if the vertex is elided due to sharing
            if (vid >= numInternalVertices)
            {
                // external vertex
                std::pair<Chunk::vertex_id_map_type::iterator, bool> added;
                added = chunk.vertexIdMap.insert(
                    std::make_pair(mesh.vertexKeys[vid - numInternalVertices],
                                   ~chunk.numExternalVertices));
                if (added.second)
                {
                    chunk.numExternalVertices++;
                    clumpExternalVertices++;
                }
                else
                    elide = true;
                tmpVertexLabel[vid] = added.first->second;
            }
            else
            {
                // internal vertex
                tmpVertexLabel[vid] = nextVertex - lastVertex;
                clumpInternalVertices++;
            }

            if (!elide)
                verticesBuffer.push_back(mesh.vertices[vid]);

            nextVertex++;
        } while (nextVertex < numVertices && globalClumpId[tmpVertexOrder[nextVertex]] == cid);

        // tmpVertexLabel now contains the intermediate encoded ID for each vertex,
        // indexed by the original (rather than permuted) ID. Transform and emit
        // the triangles using this mapping.
        while (nextTriangle < mesh.triangles.size()
               && globalClumpId[mesh.triangles[nextTriangle][0]] == cid)
        {
            boost::array<std::tr1::uint32_t, 3> out;
            for (int j = 0; j < 3; j++)
                out[j] = tmpVertexLabel[mesh.triangles[nextTriangle][j]];
            trianglesBuffer.push_back(out);
            nextTriangle++;
        }

        std::size_t clumpTriangles = nextTriangle - lastTriangle;
        chunk.bufferedClumps.push_back(Chunk::Clump(
                verticesBuffer.size() - clumpInternalVertices - clumpExternalVertices,
                clumpInternalVertices,
                clumpExternalVertices,
                trianglesBuffer.size() - clumpTriangles,
                clumpTriangles,
                cid));
    }
}

void StxxlMesher::add(MesherWork &work)
{
    if (work.chunkId.gen >= chunks.size())
        chunks.resize(work.chunkId.gen + 1);
    Chunk &chunk = chunks[work.chunkId.gen];
    chunk.chunkId = work.chunkId;

    HostKeyMesh &mesh = work.mesh;

    if (work.hasEvents)
        work.trianglesEvent.wait();
    computeLocalComponents(mesh.vertices.size(), mesh.triangles, tmpNodes);
    updateGlobalClumps(tmpNodes, mesh.triangles, tmpClumpId);

    if (work.hasEvents)
        work.vertexKeysEvent.wait();
    updateClumpKeyMap(mesh.vertexKeys, tmpClumpId);

    if (work.hasEvents)
        work.verticesEvent.wait();
    updateLocalClumps(chunk, tmpClumpId, mesh);
}

MesherBase::InputFunctor StxxlMesher::functor(unsigned int pass)
{
    /* only one pass, so ignore it */
    (void) pass;
    assert(pass == 0);

    return boost::bind(&StxxlMesher::add, this, _1);
}

void StxxlMesher::TriangleBuffer::flush()
{
    writer.writeTriangles(nextTriangle, buffer.size(), &buffer[0][0]);
    nextTriangle += buffer.size();
    buffer.clear();
}

void StxxlMesher::write(std::ostream *progressStream)
{
    FastPly::WriterBase &writer = getWriter();

    Statistics::Registry &registry = Statistics::Registry::getInstance();

    flushBuffer();

    // Number of vertices in the mesh, not double-counting chunk boundaries
    std::tr1::uint64_t totalVertices = 0;
    BOOST_FOREACH(const Clump &clump, clumps)
    {
        if (clump.isRoot())
        {
            totalVertices += clump.vertices;
        }
    }
    const std::tr1::uint64_t thresholdVertices = std::tr1::uint64_t(totalVertices * getPruneThreshold());

    clump_id keptComponents = 0, totalComponents = 0;
    std::tr1::uint64_t keptTriangles = 0;
    std::tr1::uint64_t keptVertices = 0;
    BOOST_FOREACH(const Clump &clump, clumps)
    {
        if (clump.isRoot())
        {
            totalComponents++;
            if (clump.vertices >= thresholdVertices)
            {
                keptComponents++;
                keptVertices += clump.vertices;
                keptTriangles += clump.triangles;
            }
        }
    }

    registry.getStatistic<Statistics::Variable>("components.vertices.total").add(vertices.size());
    registry.getStatistic<Statistics::Variable>("components.vertices.threshold").add(thresholdVertices);
    registry.getStatistic<Statistics::Variable>("components.vertices.kept").add(keptVertices);
    registry.getStatistic<Statistics::Variable>("components.triangles.kept").add(keptTriangles);
    registry.getStatistic<Statistics::Variable>("components.total").add(totalComponents);
    registry.getStatistic<Statistics::Variable>("components.kept").add(keptComponents);
    registry.getStatistic<Statistics::Variable>("externalvertices").add(clumpIdMap.size());

    boost::scoped_ptr<ProgressDisplay> progress;
    if (progressStream != NULL)
    {
        *progressStream << "\nWriting file(s)\n";
        progress.reset(new ProgressDisplay(2 * keptTriangles, *progressStream));
    }

    const std::tr1::uint32_t badIndex = std::numeric_limits<std::tr1::uint32_t>::max();

    /* Maps from an linear enumeration of all external vertices of a chunk to
     * the final index in the file. It is badIndex for dropped vertices,
     * although that is not actually used and could be skipped. This is declared
     * outside the loop purely to facilitate memory reuse.
     */
    Statistics::Container::vector<std::tr1::uint32_t> externalRemap("mem.StxxlMesher::externalRemap");
    // Offset to first vertex of each clump in output file
    Statistics::Container::vector<std::tr1::uint32_t> startVertex("mem.StxxlMesher::startVertex");

    for (std::size_t i = 0; i < chunks.size(); i++)
    {
        startVertex.clear();
        externalRemap.clear();

        const Chunk &chunk = chunks[i];
        std::tr1::uint64_t chunkVertices = 0;
        std::tr1::uint64_t chunkTriangles = 0;
        std::size_t chunkExternal = 0;
        for (std::size_t j = 0; j < chunk.clumps.size(); j++)
        {
            const Chunk::Clump &cc = chunk.clumps[j];
            clump_id cid = UnionFind::findRoot(clumps, cc.globalId);
            if (clumps[cid].vertices >= thresholdVertices)
            {
                chunkVertices += cc.numInternalVertices + cc.numExternalVertices;
                chunkExternal += cc.numExternalVertices;
                chunkTriangles += cc.numTriangles;
            }
        }

        if (chunkVertices >= UINT32_MAX)
        {
            std::string name = getOutputName(chunk.chunkId);
            throw std::overflow_error("Too many vertices for " + name);
        }

        if (chunkTriangles > 0)
        {
            const std::string filename = getOutputName(chunk.chunkId);
            try
            {
                writer.setNumVertices(chunkVertices);
                writer.setNumTriangles(chunkTriangles);
                writer.open(filename);
                Statistics::getStatistic<Statistics::Counter>("output.files").add();

                std::tr1::uint32_t writtenVertices = 0;
                // Write out the valid vertices, simultaneously building externalRemap
                VertexBuffer vb(writer, vertices_type::block_size / sizeof(vertices_type::value_type));
                for (std::size_t j = 0; j < chunk.clumps.size(); j++)
                {
                    const Chunk::Clump &cc = chunk.clumps[j];
                    clump_id cid = UnionFind::findRoot(clumps, cc.globalId);
                    startVertex.push_back(writtenVertices);
                    if (clumps[cid].vertices >= thresholdVertices)
                    {
                        vertices_type::const_iterator v = vertices.cbegin() + cc.firstVertex;
                        for (std::size_t i = 0; i < cc.numInternalVertices; i++, ++v)
                        {
                            vb(*v);
                        }
                        writtenVertices += cc.numInternalVertices;

                        for (std::size_t i = 0; i < cc.numExternalVertices; i++, ++v)
                        {
                            externalRemap.push_back(writtenVertices);
                            vb(*v);
                            ++writtenVertices;
                        }

                        // Yes, numTriangles. That's easier to make add up to the total
                        // than vertices (which share), and still a good indicator
                        // of progress.
                        if (progress != NULL)
                            *progress += cc.numTriangles;
                    }
                    else
                    {
                        // appends n copies of badIndex
                        externalRemap.resize(externalRemap.size() + cc.numExternalVertices, badIndex);
                    }
                }
                vb.flush();

                // Now write out the triangles
                TriangleBuffer tb(writer, triangles_type::block_size / sizeof(triangles_type::value_type));
                for (std::size_t j = 0; j < chunk.clumps.size(); j++)
                {
                    const Chunk::Clump &cc = chunk.clumps[j];
                    clump_id cid = UnionFind::findRoot(clumps, cc.globalId);
                    if (clumps[cid].vertices >= thresholdVertices)
                    {
                        triangles_type::const_iterator tp = triangles.cbegin() + cc.firstTriangle;
                        for (std::size_t i = 0; i < cc.numTriangles; i++, ++tp)
                        {
                            boost::array<std::tr1::uint32_t, 3> t = *tp;
                            // Convert indices to account for compaction
                            for (int k = 0; k < 3; k++)
                            {
                                if (~t[k] < externalRemap.size())
                                {
                                    t[k] = externalRemap[~t[k]];
                                    assert(t[k] != badIndex);
                                }
                                else
                                    t[k] += startVertex[j];
                            }
                            tb(t);
                        }
                        if (progress != NULL)
                            *progress += cc.numTriangles;
                    }
                }
                tb.flush();

                writer.close();
            }
            catch (std::ios::failure &e)
            {
                throw boost::enable_error_info(e)
                    << boost::errinfo_file_name(filename)
                    << boost::errinfo_errno(errno);
            }
        }
    }
}

namespace
{

/**
 * Class implementing @ref deviceMesher.
 */
class DeviceMesher
{
private:
    const MesherBase::InputFunctor in;
    const ChunkId chunkId;

public:
    typedef void result_type;

    void operator()(
        const cl::CommandQueue &queue,
        const DeviceKeyMesh &mesh,
        const std::vector<cl::Event> *events,
        cl::Event *event) const
    {
        MesherWork work;
        std::vector<cl::Event> wait(3);
        enqueueReadMesh(queue, mesh, work.mesh, events, &wait[0], &wait[1], &wait[2]);
        CLH::enqueueMarkerWithWaitList(queue, &wait, event);

        work.chunkId = chunkId;
        work.hasEvents = true;
        work.verticesEvent = wait[0];
        work.vertexKeysEvent = wait[1];
        work.trianglesEvent = wait[2];
        in(work);
    }

    DeviceMesher(const MesherBase::InputFunctor &in, const ChunkId &chunkId)
        : in(in), chunkId(chunkId) {}
};

} // anonymous namespace

Marching::OutputFunctor deviceMesher(const MesherBase::InputFunctor &in, const ChunkId &chunkId)
{
    return DeviceMesher(in, chunkId);
}

MesherBase *createMesher(MesherType type, FastPly::WriterBase &writer, const MesherBase::Namer &namer)
{
    switch (type)
    {
    case STXXL_MESHER:  return new StxxlMesher(writer, namer);
    }
    return NULL; // should never be reached
}
