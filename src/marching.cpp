/**
 * @file
 *
 * Marching tetrahedra algorithms.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <vector>
#include <utility>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <tr1/cstdint>
#include "clh.h"
#include "marching.h"
#include "grid.h"
#include "errors.h"
#include "statistics.h"

/* Definitions of constants */
const int Marching::KEY_AXIS_BITS;
const int Marching::MAX_DIMENSION_LOG2;
const std::size_t Marching::MAX_DIMENSION;

const unsigned char Marching::edgeIndices[NUM_EDGES][2] =
{
    {0, 1},
    {0, 2},
    {0, 3},
    {1, 3},
    {2, 3},
    {0, 4},
    {0, 5},
    {1, 5},
    {4, 5},
    {0, 6},
    {2, 6},
    {4, 6},
    {0, 7},
    {1, 7},
    {2, 7},
    {3, 7},
    {4, 7},
    {5, 7},
    {6, 7}
};

const unsigned char Marching::tetrahedronIndices[NUM_TETRAHEDRA][4] =
{
    { 0, 7, 1, 3 },
    { 0, 7, 3, 2 },
    { 0, 7, 2, 6 },
    { 0, 7, 6, 4 },
    { 0, 7, 4, 5 },
    { 0, 7, 5, 1 }
};

unsigned int Marching::findEdgeByVertexIds(unsigned int v0, unsigned int v1)
{
    if (v0 > v1) std::swap(v0, v1);
    for (unsigned int i = 0; i < NUM_EDGES; i++)
        if (edgeIndices[i][0] == v0 && edgeIndices[i][1] == v1)
            return i;
    assert(false);
    return -1;
}

template<typename Iterator>
unsigned int Marching::permutationParity(Iterator first, Iterator last)
{
    unsigned int parity = 0;
    // Simple implementation, since we will only ever call it with 4
    // items.
    for (Iterator i = first; i != last; ++i)
    {
        Iterator j = i;
        for (++j; j != last; ++j)
            if (*i > *j)
                parity ^= 1;
    }
    return parity;
}

void Marching::makeTables()
{
    std::vector<cl_uchar> hVertexTable, hIndexTable;
    std::vector<cl_uint3> hKeyTable;
    std::vector<cl_uchar2> hCountTable(NUM_CUBES);
    std::vector<cl_ushort2> hStartTable(NUM_CUBES + 1);
    for (unsigned int i = 0; i < NUM_CUBES; i++)
    {
        hStartTable[i].s0 = hVertexTable.size();
        hStartTable[i].s1 = hIndexTable.size();

        /* Find triangles. For now we record triangle indices
         * as edge numbers, which we will compact later.
         */
        std::vector<cl_uchar> triangles;
        for (unsigned int j = 0; j < NUM_TETRAHEDRA; j++)
        {
            // Holds a vertex index together with the inside/outside flag
            typedef std::pair<unsigned char, bool> tvtx;
            tvtx tvtxs[4];
            unsigned int outside = 0;
            // Copy the vertices to tvtxs, and count vertices that are external
            for (unsigned int k = 0; k < 4; k++)
            {
                unsigned int v = tetrahedronIndices[j][k];
                bool o = (i & (1 << v));
                outside += o;
                tvtxs[k] = tvtx(v, o);
            }
            unsigned int baseParity = permutationParity(tvtxs, tvtxs + 4);

            // Reduce number of cases to handle by flipping inside/outside to
            // ensure that outside <= 2.
            if (outside > 2)
            {
                // Causes triangle winding to flip as well - otherwise
                // the triangle will be inside out.
                baseParity ^= 1;
                for (unsigned int k = 0; k < 4; k++)
                    tvtxs[k].second = !tvtxs[k].second;
            }

            /* To reduce the number of cases to handle, the tetrahedron is
             * rotated to match one of the canonical configurations (all
             * inside, v0 outside, (v0, v1) outside). There are 24 permutations
             * of the vertices, half of which are rotations and half of which are
             * reflections. Not all of them need to be tried, but this code isn't
             * performance critical.
             */
            sort(tvtxs, tvtxs + 4);
            do
            {
                // Check that it is a rotation rather than a reflection
                if (permutationParity(tvtxs, tvtxs + 4) == baseParity)
                {
                    const unsigned int t0 = tvtxs[0].first;
                    const unsigned int t1 = tvtxs[1].first;
                    const unsigned int t2 = tvtxs[2].first;
                    const unsigned int t3 = tvtxs[3].first;
                    unsigned int mask = 0;
                    for (unsigned int k = 0; k < 4; k++)
                        mask |= tvtxs[k].second << k;
                    if (mask == 0)
                    {
                        break; // no outside vertices, so no triangles needed
                    }
                    else if (mask == 1)
                    {
                        // One outside vertex, one triangle needed
                        triangles.push_back(findEdgeByVertexIds(t0, t1));
                        triangles.push_back(findEdgeByVertexIds(t0, t3));
                        triangles.push_back(findEdgeByVertexIds(t0, t2));
                        break;
                    }
                    else if (mask == 3)
                    {
                        // Two outside vertices, two triangles needed to tile a quad
                        triangles.push_back(findEdgeByVertexIds(t0, t2));
                        triangles.push_back(findEdgeByVertexIds(t1, t2));
                        triangles.push_back(findEdgeByVertexIds(t1, t3));

                        triangles.push_back(findEdgeByVertexIds(t1, t3));
                        triangles.push_back(findEdgeByVertexIds(t0, t3));
                        triangles.push_back(findEdgeByVertexIds(t0, t2));
                        break;
                    }
                }
            } while (next_permutation(tvtxs, tvtxs + 4));
        }

        // Determine which edges are in use, and assign indices
        int edgeCompact[NUM_EDGES];
        int pool = 0;
        for (unsigned int j = 0; j < NUM_EDGES; j++)
        {
            if (std::count(triangles.begin(), triangles.end(), j))
            {
                edgeCompact[j] = pool++;
                hVertexTable.push_back(j);
                cl_uint3 key = { {0, 0, 0} };
                for (unsigned int axis = 0; axis < 3; axis++)
                {
                    key.s[axis] =
                        ((edgeIndices[j][0] >> axis) & 1)
                        + ((edgeIndices[j][1] >> axis) & 1);
                }
                hKeyTable.push_back(key);
            }
        }
        for (unsigned int j = 0; j < triangles.size(); j++)
        {
            hIndexTable.push_back(edgeCompact[triangles[j]]);
        }

        hCountTable[i].s0 = hVertexTable.size() - hStartTable[i].s0;
        hCountTable[i].s1 = hIndexTable.size() - hStartTable[i].s1;
    }

    hStartTable[NUM_CUBES].s0 = hVertexTable.size();
    hStartTable[NUM_CUBES].s1 = hIndexTable.size();

    /* We're going to concatenate hVertexTable and hIndexTable, so the start values
     * need to be offset to point to where hIndexTable sits afterwards.
     */
    for (unsigned int i = 0; i <= NUM_CUBES; i++)
    {
        hStartTable[i].s1 += hVertexTable.size();
    }
    // Concatenate the two tables into one
    hVertexTable.insert(hVertexTable.end(), hIndexTable.begin(), hIndexTable.end());

    countTable = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                            hCountTable.size() * sizeof(hCountTable[0]), &hCountTable[0]);
    startTable = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                            hStartTable.size() * sizeof(hStartTable[0]), &hStartTable[0]);
    dataTable =  cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                            hVertexTable.size() * sizeof(hVertexTable[0]), &hVertexTable[0]);
    keyTable =   cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                            hKeyTable.size() * sizeof(hKeyTable[0]), &hKeyTable[0]);
}

bool Marching::validateDevice(const cl::Device &device)
{
    if (!device.getInfo<CL_DEVICE_IMAGE_SUPPORT>())
        return false;
    return true;
}

std::tr1::uint64_t Marching::getMaxVertices(std::size_t maxWidth, std::size_t maxHeight)
{
    return std::tr1::uint64_t(maxWidth - 1) * (maxHeight - 1) * MAX_CELL_VERTICES;
}

std::tr1::uint64_t Marching::getMaxTriangles(std::size_t maxWidth, std::size_t maxHeight)
{
    return std::tr1::uint64_t(maxWidth - 1) * (maxHeight - 1) * (MAX_CELL_INDICES / 3);
}

CLH::ResourceUsage Marching::resourceUsage(const cl::Device &device, std::size_t maxWidth, std::size_t maxHeight)
{
    MLSGPU_ASSERT(maxWidth <= MAX_DIMENSION, std::invalid_argument);
    MLSGPU_ASSERT(maxHeight <= MAX_DIMENSION, std::invalid_argument);
    (void) device; // not currently used, but should be used to determine usage of clogs

    CLH::ResourceUsage ans;

    // The asserts above guarantee that these will not overflow
    const std::tr1::uint64_t sliceCells = (maxWidth - 1) * (maxHeight - 1);
    const std::tr1::uint64_t vertexSpace = getMaxVertices(maxWidth, maxHeight);
    const std::tr1::uint64_t indexSpace = getMaxTriangles(maxWidth, maxHeight) * 3;

    // Keep this in sync with the actual allocations below

    for (unsigned int i = 0; i < 2; i++)
    {
        // backingImages[i] = cl::Image2D(context, CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), maxWidth, maxHeight);
        ans.addImage(maxWidth, maxHeight, sizeof(cl_float));
    }

    // cells = cl::Buffer(context, CL_MEM_READ_WRITE, sliceCells * sizeof(cl_uint2));
    ans.addBuffer(sliceCells * sizeof(cl_uint2));

    // occupied = cl::Buffer(context, CL_MEM_READ_WRITE, (sliceCells + 1) * sizeof(cl_uint));
    ans.addBuffer((sliceCells + 1) * sizeof(cl_uint));

    // viCount = cl::Buffer(context, CL_MEM_READ_WRITE, (sliceCells + 1) * sizeof(cl_uint2));
    ans.addBuffer((sliceCells + 1) * sizeof(cl_uint2));

    // vertexUnique = cl::Buffer(context, CL_MEM_READ_WRITE, (vertexSpace + 1) * sizeof(cl_uint));
    ans.addBuffer((vertexSpace + 1) * sizeof(cl_uint));

    // indexRemap = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_uint));
    ans.addBuffer(vertexSpace * sizeof(cl_uint));

    // unweldedVertices = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_float4));
    // unweldedVertexKeys = cl::Buffer(context, CL_MEM_READ_WRITE, (vertexSpace + 1) * sizeof(cl_ulong));
    ans.addBuffer(vertexSpace * sizeof(cl_float4));
    ans.addBuffer((vertexSpace + 1) * sizeof(cl_ulong));

    // weldedVertices = cl::Buffer(context, CL_MEM_WRITE_ONLY, vertexSpace * 3 * sizeof(cl_float));
    // weldedVertexKeys = cl::Buffer(context, CL_MEM_WRITE_ONLY, vertexSpace * sizeof(cl_ulong));
    ans.addBuffer(vertexSpace * 3 * sizeof(cl_float));
    ans.addBuffer(vertexSpace * sizeof(cl_ulong));

    // indices = cl::Buffer(context, CL_MEM_READ_WRITE, indexSpace * sizeof(cl_uint));
    ans.addBuffer(indexSpace * sizeof(cl_uint));

    // firstExternal = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(cl_uint));
    ans.addBuffer(sizeof(cl_uint));

    // tmpVertexKeys = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_ulong));
    // tmpVertices = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_float4));
    ans.addBuffer(vertexSpace * sizeof(cl_ulong));
    ans.addBuffer(vertexSpace * sizeof(cl_float4));

    // TODO: constant space needed for tables, and temporaries for the sorter and scanners

    return ans;
}

Marching::Marching(const cl::Context &context, const cl::Device &device,
                   size_t maxWidth, size_t maxHeight)
:
    maxWidth(maxWidth), maxHeight(maxHeight), context(context),
    scanUint(context, device, clogs::TYPE_UINT),
    scanElements(context, device, clogs::Type(clogs::TYPE_UINT, 2)),
    sortVertices(context, device, clogs::TYPE_ULONG, clogs::Type(clogs::TYPE_FLOAT, 4))
{
    MLSGPU_ASSERT(2 <= maxWidth && maxWidth <= MAX_DIMENSION, std::invalid_argument);
    MLSGPU_ASSERT(2 <= maxHeight && maxHeight <= MAX_DIMENSION, std::invalid_argument);

    makeTables();
    for (unsigned int i = 0; i < 2; i++)
        backingImages[i] = cl::Image2D(context, CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), maxWidth, maxHeight);

    const std::size_t sliceCells = (maxWidth - 1) * (maxHeight - 1);
    vertexSpace = sliceCells * MAX_CELL_VERTICES;
    indexSpace = sliceCells * MAX_CELL_INDICES;

    // If these are updated, also update deviceMemory
    cells = cl::Buffer(context, CL_MEM_READ_WRITE, sliceCells * sizeof(cl_uint2));
    occupied = cl::Buffer(context, CL_MEM_READ_WRITE, (sliceCells + 1) * sizeof(cl_uint));
    viCount = cl::Buffer(context, CL_MEM_READ_WRITE, (sliceCells + 1) * sizeof(cl_uint2));
    vertexUnique = cl::Buffer(context, CL_MEM_READ_WRITE, (vertexSpace + 1) * sizeof(cl_uint));
    indexRemap = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_uint));
    unweldedVertices = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_float4));
    unweldedVertexKeys = cl::Buffer(context, CL_MEM_READ_WRITE, (vertexSpace + 1) * sizeof(cl_ulong));
    weldedVertices = cl::Buffer(context, CL_MEM_WRITE_ONLY, vertexSpace * 3 * sizeof(cl_float));
    weldedVertexKeys = cl::Buffer(context, CL_MEM_WRITE_ONLY, vertexSpace * sizeof(cl_ulong));
    indices = cl::Buffer(context, CL_MEM_READ_WRITE, indexSpace * sizeof(cl_uint));
    firstExternal = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(cl_uint));
    tmpVertexKeys = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_ulong));
    tmpVertices = cl::Buffer(context, CL_MEM_READ_WRITE, vertexSpace * sizeof(cl_float4));
    sortVertices.setTemporaryBuffers(tmpVertexKeys, tmpVertices);

    program = CLH::build(context, std::vector<cl::Device>(1, device), "kernels/marching.cl");
    countOccupiedKernel = cl::Kernel(program, "countOccupied");
    compactKernel = cl::Kernel(program, "compact");
    countElementsKernel = cl::Kernel(program, "countElements");
    generateElementsKernel = cl::Kernel(program, "generateElements");
    countUniqueVerticesKernel = cl::Kernel(program, "countUniqueVertices");
    compactVerticesKernel = cl::Kernel(program, "compactVertices");
    reindexKernel = cl::Kernel(program, "reindex");

    // Set up kernel arguments that never change.
    countOccupiedKernel.setArg(0, occupied);

    compactKernel.setArg(0, cells);
    compactKernel.setArg(1, occupied);

    countElementsKernel.setArg(0, viCount);
    countElementsKernel.setArg(1, cells);
    countElementsKernel.setArg(4, countTable);

    generateElementsKernel.setArg(0, unweldedVertices);
    generateElementsKernel.setArg(1, unweldedVertexKeys);
    generateElementsKernel.setArg(2, indices);
    generateElementsKernel.setArg(3, viCount);
    generateElementsKernel.setArg(4, cells);
    generateElementsKernel.setArg(7, startTable);
    generateElementsKernel.setArg(8, dataTable);
    generateElementsKernel.setArg(9, keyTable);

    countUniqueVerticesKernel.setArg(0, vertexUnique);
    countUniqueVerticesKernel.setArg(1, unweldedVertexKeys);

    compactVerticesKernel.setArg(0, weldedVertices);
    compactVerticesKernel.setArg(1, weldedVertexKeys);
    compactVerticesKernel.setArg(2, indexRemap);
    compactVerticesKernel.setArg(3, firstExternal);
    compactVerticesKernel.setArg(4, vertexUnique);
    compactVerticesKernel.setArg(5, unweldedVertices);
    compactVerticesKernel.setArg(6, unweldedVertexKeys);

    reindexKernel.setArg(0, indices);
    reindexKernel.setArg(1, indexRemap);
}

std::size_t Marching::generateCells(const cl::CommandQueue &queue,
                                    const cl::Image2D &sliceA,
                                    const cl::Image2D &sliceB,
                                    std::size_t width, std::size_t height,
                                    const std::vector<cl::Event> *events)
{
    cl::Event last;
    const std::size_t levelCells = (width - 1) * (height - 1);

    countOccupiedKernel.setArg(1, sliceA);
    countOccupiedKernel.setArg(2, sliceB);
    queue.enqueueNDRangeKernel(countOccupiedKernel,
                               cl::NullRange,
                               cl::NDRange(width - 1, height - 1),
                               cl::NullRange,
                               events, &last);

    std::vector<cl::Event> wait(1);
    wait[0] = last;
    scanUint.enqueue(queue, occupied, levelCells + 1, NULL, &wait, &last);
    wait[0] = last;

    cl_uint compacted;
    queue.enqueueReadBuffer(occupied, CL_FALSE, levelCells * sizeof(cl_uint), sizeof(cl_uint), &compacted,
                            &wait, NULL);

    // In parallel to the readback, do compaction
    queue.enqueueNDRangeKernel(compactKernel,
                               cl::NullRange,
                               cl::NDRange(width - 1, height - 1),
                               cl::NullRange,
                               &wait, NULL);

    // Now obtain the number of compacted cells for subsequent steps
    queue.finish();
    return compacted;
}

cl_uint2 Marching::countElements(const cl::CommandQueue &queue,
                                 const cl::Image2D &sliceA,
                                 const cl::Image2D &sliceB,
                                 std::size_t compacted,
                                 const std::vector<cl::Event> *events)
{
    cl::Event last;
    std::vector<cl::Event> wait(1);

    countElementsKernel.setArg(2, sliceA);
    countElementsKernel.setArg(3, sliceB);
    CLH::enqueueNDRangeKernel(queue,
                              countElementsKernel,
                              cl::NullRange,
                              cl::NDRange(compacted),
                              cl::NullRange,
                              events, &last);
    wait[0] = last;

    scanElements.enqueue(queue, viCount, compacted + 1, NULL, &wait, &last);
    wait[0] = last;

    cl_uint2 ans;
    queue.enqueueReadBuffer(viCount, CL_TRUE, compacted * sizeof(cl_uint2), sizeof(cl_uint2),
                            &ans, &wait, NULL);
    return ans;
}

void Marching::shipOut(const cl::CommandQueue &queue,
                       const cl_uint3 &keyOffset,
                       const cl_uint2 &sizes,
                       cl_uint zMax,
                       const OutputFunctor &output,
                       const std::vector<cl::Event> *events,
                       cl::Event *event)
{
    std::vector<cl::Event> wait(1);
    cl::Event last;

    // Write a sentinel key after the real vertex keys
    cl_ulong key = CL_ULONG_MAX;
    queue.enqueueWriteBuffer(unweldedVertexKeys, CL_FALSE, sizes.s0 * sizeof(cl_ulong), sizeof(cl_ulong), &key,
                             events, &last);
    wait[0] = last;

    // TODO: figure out how many actual bits there are
    // TODO: revisit the dependency tracking
    sortVertices.enqueue(queue, unweldedVertexKeys, unweldedVertices, sizes.s0, 0, &wait, &last);
    wait[0] = last;

    CLH::enqueueNDRangeKernel(queue,
                              countUniqueVerticesKernel,
                              cl::NullRange,
                              cl::NDRange(sizes.s0),
                              cl::NullRange,
                              &wait, &last);
    wait[0] = last;

    scanUint.enqueue(queue, vertexUnique, sizes.s0 + 1, NULL, &wait, &last);
    wait[0] = last;

    // Start this readback - but we don't immediately need the result.
    cl_uint numWelded;
    queue.enqueueReadBuffer(vertexUnique, CL_FALSE, sizes.s0 * sizeof(cl_uint), sizeof(cl_uint), &numWelded, &wait, NULL);

    // TODO: should we be sorting key/value pairs? The values are going to end up moving
    // twice, and most of them will be eliminated entirely! However, sorting them does
    // give later passes better spatial locality and fewer indirections.
    cl_ulong minExternalKey = cl_ulong(zMax) << (2 * KEY_AXIS_BITS + 1);
    cl_ulong keyOffsetL =
        (cl_ulong(keyOffset.z) << (2 * KEY_AXIS_BITS + 1))
        | (cl_ulong(keyOffset.y) << (KEY_AXIS_BITS + 1))
        | (cl_ulong(keyOffset.x) << 1);
    compactVerticesKernel.setArg(7, minExternalKey);
    compactVerticesKernel.setArg(8, keyOffsetL);
    CLH::enqueueNDRangeKernel(queue,
                              compactVerticesKernel,
                              cl::NullRange,
                              cl::NDRange(sizes.s0),
                              cl::NullRange,
                              &wait, &last);
    wait[0] = last;

    cl_uint hFirstExternal;
    queue.enqueueReadBuffer(firstExternal, CL_FALSE, 0, sizeof(cl_uint), &hFirstExternal, &wait, NULL);

    CLH::enqueueNDRangeKernel(queue,
                              reindexKernel,
                              cl::NullRange,
                              cl::NDRange(sizes.s1),
                              cl::NullRange,
                              &wait, &last);
    queue.finish(); // wait for readback of numWelded and firstExternal (TODO: overkill)

    DeviceKeyMesh outputMesh; // TODO: store buffers in this instead of copying references
    outputMesh.vertices = weldedVertices;
    outputMesh.vertexKeys = weldedVertexKeys;
    outputMesh.triangles = indices;
    outputMesh.numVertices = numWelded;
    outputMesh.numInternalVertices = hFirstExternal;
    outputMesh.numTriangles = sizes.s1 / 3;
    output(queue, outputMesh, NULL, event);
}

void Marching::generate(
    const cl::CommandQueue &queue,
    const InputFunctor &input,
    const OutputFunctor &output,
    const Grid::size_type size[3],
    const cl_uint3 &keyOffset,
    const std::vector<cl::Event> *events)
{
    Statistics::Registry &registry = Statistics::Registry::getInstance();
    Statistics::Variable &nonempty = registry.getStatistic<Statistics::Variable>("marching.slice.nonempty");

    // Work group size for kernels that operate on compacted cells
    const std::size_t wgsCompacted = 1; // TODO: not very good at all!

    // Pointers into @ref backingImages, which are swapped to advance to the next slice.
    cl::Image2D *images[2] = { &backingImages[0], &backingImages[1] };

    const Grid::size_type width = size[0];
    const Grid::size_type height = size[1];
    const Grid::size_type depth = size[2];
    MLSGPU_ASSERT(1U <= width && width <= maxWidth, std::length_error);
    MLSGPU_ASSERT(1U <= height && height <= maxHeight, std::length_error);
    MLSGPU_ASSERT(1U <= depth, std::length_error);

    std::vector<cl::Event> wait(1);
    cl::Event last, readEvent;
    cl_uint2 offsets = { {0, 0} };
    cl_uint3 top = { {2 * (width - 1), 2 * (height - 1), 0} };

    input(queue, *images[1], 0, events, &last);
    wait[0] = last;

    Grid::size_type shipOuts = 0;
    for (Grid::size_type z = 1; z < depth; z++)
    {
        std::swap(images[0], images[1]);
        input(queue, *images[1], z, &wait, &last);
        wait.resize(1);
        wait[0] = last;

        std::size_t compacted = generateCells(queue, *images[0], *images[1],
                                              width, height, &wait);
        wait.clear();
        if (compacted > 0)
        {
            cl_uint2 counts = countElements(queue, *images[0], *images[1], compacted, events);
            if (offsets.s0 + counts.s0 > vertexSpace
                || offsets.s1 + counts.s1 > indexSpace)
            {
                /* Too much information in this layer to just append. Ship out
                 * what we have before processing this layer.
                 */
                shipOut(queue, keyOffset, offsets, z - 1, output, &wait, &last);
                shipOuts++;
                wait.resize(1);
                wait[0] = last;

                offsets.s0 = 0;
                offsets.s1 = 0;
                top.z = 2 * (z - 1);

                /* We'd better have enough room to process one layer at a time.
                 */
                assert(counts.s0 <= vertexSpace);
                assert(counts.s1 <= indexSpace);
            }

            generateElementsKernel.setArg(5, *images[0]);
            generateElementsKernel.setArg(6, *images[1]);
            generateElementsKernel.setArg(10, cl_uint(z - 1));
            generateElementsKernel.setArg(11, keyOffset);
            generateElementsKernel.setArg(12, offsets);
            generateElementsKernel.setArg(13, top);
            generateElementsKernel.setArg(14, cl::__local(NUM_EDGES * wgsCompacted * sizeof(cl_float3)));
            CLH::enqueueNDRangeKernel(queue,
                                      generateElementsKernel,
                                      cl::NullRange,
                                      cl::NDRange(compacted),
                                      cl::NDRange(wgsCompacted),
                                      &wait, &last);
            wait.resize(1);
            wait[0] = last;

            offsets.s0 += counts.s0;
            offsets.s1 += counts.s1;
        }
        nonempty.add(compacted > 0);
    }
    if (offsets.s0 > 0)
    {
        shipOut(queue, keyOffset, offsets, depth - 1, output, &wait, &last);
        shipOuts++;
        wait.resize(1);
        wait[0] = last;
    }
    if (shipOuts > 0)
        registry.getStatistic<Statistics::Variable>("marching.shipouts").add(shipOuts);
    queue.finish(); // will normally be finished already, but there may be corner cases
}
