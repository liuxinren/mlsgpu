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
 * Test code for @ref SplatSet::FastBlobSetMPI.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/iostreams/device/null.hpp>
#include <boost/iostreams/stream.hpp>
#include <memory>
#include <vector>
#include <string>
#include <mpi.h>
#include "../test_splat_set.h"
#include "../testutil.h"
#include "../../src/grid.h"
#include "../../src/splat.h"
#include "../../src/splat_set_mpi.h"

using namespace SplatSet;

/// Tests for @ref SplatSet::FastBlobSetMPI <SplatSet::FileSet>.
class TestFastFileSetMPI : public TestFastFileSet
{
    CPPUNIT_TEST_SUB_SUITE(TestFastFileSetMPI, TestFastFileSet);
#if DEBUG
    CPPUNIT_TEST(testEmpty);
#endif
    CPPUNIT_TEST(testProgress);
    CPPUNIT_TEST_SUITE_END();

private:
    MPI_Comm comm;
    std::vector<std::string> store;

protected:
    virtual Set *setFactory(const std::vector<std::vector<Splat> > &splatData,
                            float spacing, Grid::size_type bucketSize);
public:
    virtual void setUp();
    virtual void tearDown();

    void testEmpty();            ///< Test error checking for an empty set
    void testProgress();         ///< Run with a progress stream (does not check output)
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestFastFileSetMPI, TestSet::perBuild());

void TestFastFileSetMPI::setUp()
{
    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Comm_dup(MPI_COMM_WORLD, &comm);
    TestFastFileSet::setUp();
}

void TestFastFileSetMPI::tearDown()
{
    TestFastFileSet::tearDown();
    MPI_Comm_free(&comm);
    MPI_Barrier(MPI_COMM_WORLD);
}

TestFastFileSetMPI::Set *TestFastFileSetMPI::setFactory(
    const std::vector<std::vector<Splat> > &splatData,
    float spacing, Grid::size_type bucketSize)
{
    if (splatData.empty())
        return NULL; // otherwise computeBlobs will throw
    std::auto_ptr<FastBlobSetMPI<FileSet> > set(new FastBlobSetMPI<FileSet>);
    TestFileSet::populate(*set, splatData, store);
    set->computeBlobs(comm, 0, spacing, bucketSize, NULL, false);
    return set.release();
}

void TestFastFileSetMPI::testEmpty()
{
    boost::scoped_ptr<FastBlobSetMPI<FileSet> > set(new FastBlobSetMPI<FileSet>());
    CPPUNIT_ASSERT_THROW(set->computeBlobs(comm, 0, 2.5f, 5, NULL, false), std::runtime_error);
}

void TestFastFileSetMPI::testProgress()
{
    boost::scoped_ptr<FastBlobSetMPI<FileSet> > set(new FastBlobSetMPI<FileSet>());
    TestFileSet::populate(*set, splatData, store);

    boost::iostreams::null_sink nullSink;
    boost::iostreams::stream<boost::iostreams::null_sink> nullStream(nullSink);
    set->computeBlobs(comm, 0, 2.5f, 5, &nullStream, false);
}
