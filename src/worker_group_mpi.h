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
 * Thread pool classes for worker/slave in MPI.
 */

#ifndef WORKER_GROUP_MPI_H
#define WORKER_GROUP_MPI_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <mpi.h>
#include <cassert>
#include "worker_group.h"
#include "tags.h"
#include "serialize.h"

/**
 * Transmits an item by calling its @c send member. For items that do not have this
 * member, this template can be specialized.
 */
template<typename Item>
void sendItem(const Item &item, MPI_Comm comm, int dest)
{
    item.send(comm, dest);
}

/**
 * Receives an item by calling its @c recv member. For items that do not have this
 * member, this template can be specialized.
 */
template<typename Item>
void recvItem(Item &item, MPI_Comm comm, int source)
{
    item.recv(comm, source);
}

/**
 * Determines a size for an item, that is passed to @ref WorkerGroup::get on the
 * receiver to allocate storage for the item. The default implementation is to
 * call a @a size member. For items that do not have this member, this template
 * can be specialized.
 */
template<typename Item>
std::size_t sizeItem(const Item &item)
{
    return item.size();
}


/**
 * A worker that is suitable for use with @ref WorkerGroupGather. When it pulls
 * an item from the queue, it first informs the remote that it has some work,
 * then sends it. When the queue is drained, it instead tells the remote to
 * shut down.
 */
template<typename WorkItem>
class WorkerGather : public WorkerBase
{
private:
    MPI_Comm comm;
    int root;
    Statistics::Variable &sendStat;
public:
    /**
     * Constructor.
     *
     * @param name      Name for the worker.
     * @param comm      Communicator to communicate with the remote end.
     * @param root      Target for messages.
     * @param sendStat  Statistic for time spent sending
     */
    WorkerGather(const std::string &name, MPI_Comm comm, int root, Statistics::Variable &sendStat)
        : WorkerBase(name, 0), comm(comm), root(root), sendStat(sendStat)
    {
    }

    void operator()(WorkItem &item)
    {
        Timeplot::Action action("send", getTimeplotWorker(), sendStat);
        std::size_t workSize = sizeItem(item);
        MPI_Send(&workSize, 1, Serialize::mpi_type_traits<std::size_t>::type(), root,
                 MLSGPU_TAG_GATHER_HAS_WORK, comm);
        sendItem(item, comm, root);
    }

    void stop()
    {
        std::size_t workSize = 0;
        MPI_Send(&workSize, 1, Serialize::mpi_type_traits<std::size_t>::type(), root,
                 MLSGPU_TAG_GATHER_HAS_WORK, comm);
    }
};

/**
 * Counterpart to @ref WorkerGather that receives the messages and
 * places them into a @ref WorkerGroup. The receiver is run by calling its
 * <code>operator()</code> (it may thus be used with @c boost::thread). When
 * there is no more data to receive it will terminate, although it will not
 * stop the group it is feeding.
 */
template<typename WorkItem, typename Group>
class ReceiverGather : public boost::noncopyable
{
private:
    Group &outGroup;
    const MPI_Comm comm;
    const std::size_t senders;
    Timeplot::Worker tworker;

public:
    ReceiverGather(const std::string &name, Group &outGroup, MPI_Comm comm, std::size_t senders)
        : outGroup(outGroup), comm(comm), senders(senders), tworker(name)
    {
    }

    void operator()()
    {
        std::size_t rem = senders;
        Statistics::Variable &waitStat = Statistics::getStatistic<Statistics::Variable>("ReceiverGather.wait");
        Statistics::Variable &recvStat = Statistics::getStatistic<Statistics::Variable>("ReceiverGather.recv");
        while (rem > 0)
        {
            std::size_t workSize;
            MPI_Status status;
            {
                Timeplot::Action action("wait", tworker, waitStat);
                MPI_Recv(&workSize, 1, Serialize::mpi_type_traits<std::size_t>::type(),
                         MPI_ANY_SOURCE, MLSGPU_TAG_GATHER_HAS_WORK, comm, &status);
            }
            if (workSize == 0)
                rem--;
            else
            {
                boost::shared_ptr<WorkItem> item = outGroup.get(tworker, workSize);
                {
                    Timeplot::Action action("recv", tworker, recvStat);
                    recvItem(*item, comm, status.MPI_SOURCE);
                }
                outGroup.push(tworker, item);
            }
        }
    }
};

/**
 * Worker group that handles sending items from a queue to a @ref
 * ReceiverGather running on another MPI process.
 */
template<typename WorkItem, typename Derived>
class WorkerGroupGather : public WorkerGroup<WorkItem, WorkerGather<WorkItem>, Derived>
{
protected:
    /**
     * Constructor. This takes care of constructing the (single) worker.
     *
     * @param name      Name for the group (also for the worker).
     * @param comm      Communicator to send the items.
     * @param root      Destination for the items within @a comm.
     */
    WorkerGroupGather(const std::string &name, MPI_Comm comm, int root)
        : WorkerGroup<WorkItem, WorkerGather<WorkItem>, Derived>(name, 1)
    {
        this->addWorker(new WorkerGather<WorkItem>(name, comm, root, this->getComputeStat()));
    }
};

#endif /* WORKER_GROUP_MPI_H */
