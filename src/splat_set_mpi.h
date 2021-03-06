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
 * Computation of blobs using multiple nodes.
 */

#ifndef SPLAT_SET_MPI_H
#define SPLAT_SET_MPI_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <mpi.h>
#include <ostream>
#include <utility>
#include <memory>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/thread/thread.hpp>
#include <boost/ref.hpp>
#include "grid.h"
#include "splat_set.h"
#include "progress.h"
#include "progress_mpi.h"
#include "errors.h"
#include "serialize.h"

namespace SplatSet
{

template<typename Base>
class FastBlobSetMPI : public FastBlobSet<Base>
{
public:
    /**
     * Computes the blobs for the underlying set collectively across all ranks.
     * The results are broadcast back to the ranks.
     *
     * @param comm           Communicator for the collective operation.
     * @param root           Master for the collective operation (affects logging only)
     * @param spacing        Grid spacing for grids to be accelerated.
     * @param bucketSize     Common factor for bucket sizes to be accelerated.
     * @param progressStream If non-NULL, will be used to report collective progress
     * @param warnNonFinite  If true (the default), a warning will be displayed if
     *                       non-finite splats are encountered.
     *
     * @pre
     * - The underlying set of splats is identical at all ranks.
     * - All ranks specify the same value for @a root, @a spacing and @a bucketSize
     *
     * @note The progress is actually written to the stream on the root, but
     * either ranks must pass NULL for @a progressStream or all ranks must pass
     * non-NULL.
     */
    void computeBlobs(
        MPI_Comm comm, int root,
        float spacing, Grid::size_type bucketSize,
        std::ostream *progressStream = NULL,
        bool warnNonFinite = true);
};

template<typename Base>
void FastBlobSetMPI<Base>::computeBlobs(
    MPI_Comm comm, int root,
    float spacing, Grid::size_type bucketSize,
    std::ostream *progressStream,
    bool warnNonFinite)
{
    int rank, size;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);

    Statistics::Registry &registry = Statistics::Registry::getInstance();

    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    this->internalBucketSize = bucketSize;
    this->eraseBlobFiles();
    this->blobFiles.reserve(size);
    this->nSplats = 0;
    detail::Bbox bbox;

    boost::scoped_ptr<ProgressDisplay> progressDisplay;
    boost::scoped_ptr<ProgressMPI> progress;
    boost::scoped_ptr<boost::thread> progressThread;
    if (progressStream != NULL)
    {
        if (rank == root)
        {
            *progressStream << "Computing bounding box\n";
            progressDisplay.reset(new ProgressDisplay(Base::maxSplats(), *progressStream));
        }
        // On non-root node this will pass a NULL upstream, which is correct
        progress.reset(new ProgressMPI(progressDisplay.get(), Base::maxSplats(), comm, root));
        if (rank == root)
            progressThread.reset(new boost::thread(boost::ref(*progress)));
    }

    typename FastBlobSet<Base>::BlobFile blobFile; // TODO: exception safety
    try
    {
        const detail::SplatToBuckets toBuckets(spacing, bucketSize);
        std::pair<splat_id, splat_id> range = Base::partition(rank, size);
        this->computeBlobsRange(
            range.first, range.second,
            toBuckets,
            bbox, blobFile, this->nSplats,
            progress.get());

        MPI_Allreduce(MPI_IN_PLACE, &this->nSplats, 1, Serialize::mpi_type_traits<splat_id>::type(), MPI_SUM, comm);
        MPI_Allreduce(MPI_IN_PLACE, &bbox.bboxMin[0], 3, MPI_FLOAT, MPI_MIN, comm);
        MPI_Allreduce(MPI_IN_PLACE, &bbox.bboxMax[0], 3, MPI_FLOAT, MPI_MAX, comm);

        assert(this->nSplats <= Base::maxSplats());
        if (progress)
            progress->sync();
        if (rank == root)
        {
            splat_id nonFinite = Base::maxSplats() - this->nSplats;
            if (progressThread)
            {
                *progress += nonFinite;
                progress->sync();
                progressThread->join();
                progressThread.reset();
            }
            if (nonFinite > 0 && warnNonFinite)
            {
                Log::log[Log::warn] << "Input contains " << nonFinite << " splat(s) with non-finite values\n";
            }
            registry.getStatistic<Statistics::Variable>("blobset.nonfinite").add(nonFinite);
        }
        this->boundingGrid = this->makeBoundingGrid(spacing, bucketSize, bbox);

        /* Distribute the filenames. This is not done with MPI_Alltoall since that requires
         * placing all the filenames in a single buffer.
         */
        for (int i = 0; i < size; i++)
        {
            std::tr1::uint64_t nBlobs = blobFile.nBlobs;
            boost::filesystem::path path = blobFile.path;

            MPI_Bcast(&nBlobs, 1, Serialize::mpi_type_traits<std::tr1::uint64_t>::type(),
                      i, comm);
            Serialize::broadcast(path, comm, i);
            this->blobFiles.push_back(typename FastBlobSet<Base>::BlobFile());
            this->blobFiles.back().path = path;
            this->blobFiles.back().nBlobs = nBlobs;
            this->blobFiles.back().owner = (rank == root);
            MPI_Barrier(comm); // ensures that the master takes ownership before the worker releases it
            if (i == rank)
                blobFile.owner = false;
        }
    }
    catch (std::exception &e)
    {
        this->eraseBlobFile(blobFile);
        throw;
    }
}

} // namespace SplatSet

#endif /* !SPLAT_SET_MPI_H */
