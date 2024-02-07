// Copyright (c) 2023 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================================
#include <cstdlib>
#include <mpi.h>
#include "oneapi/ccl.hpp"

static ccl::communicator *pcomm;

extern "C" int init(int *world_size, int *world_rank, int *world_color) {
    ccl::init();

    MPI_Init(NULL, NULL);
    MPI_Comm_size(MPI_COMM_WORLD, world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, world_rank);
printf("world_size: %d, world_rank: %d, world_color: %d\n", *world_size, *world_rank, *world_color);
fflush(stdout);

    // 1) rank = 0, 1, 2, 3, 4, 5, 6, 7; pp = 2; tp = 4
    //    color = 0, 0, 0, 0, 1, 1, 1, 1
    // 2) rank = 0, 1, 2, 3, 4, 5, 6, 7; pp = 4; tp = 2
    //    color = 0, 0, 1, 1, 2, 2, 3, 3
    // 3) rank = 0, 1, 2, 3; pp = 1; tp = 4
    //    color = 0, 0, 0, 0
    // 4) rank = 0, 1, 2, 3; pp = 2; tp = 2
    //    color = 0, 0, 1, 1
    // 5) rank = 0, 1, 2, 3; pp = 4; tp = 1
    //    color = 0, 1, 2, 3
    // 7) rank = 0, 1; pp = 1; tp = 2
    //    color = 0, 0
    // 8) rank = 0, 1; pp = 2; tp = 1
    //    color = 0, 1
    // world_color = world_rank / tp_num = world_rank / (world_size / pp_num)
    *world_color = *world_rank / (*world_size / *world_color);
    MPI_Comm row_comm;
    MPI_Comm_split(MPI_COMM_WORLD, *world_color, *world_rank, &row_comm);

    int row_size, row_rank;
    MPI_Comm_size(row_comm, &row_size);
    MPI_Comm_rank(row_comm, &row_rank);
printf("row_size: %d, row_rank: %d, world_color: %d\n", row_size, row_rank, *world_color);
fflush(stdout);
    ccl::shared_ptr_class<ccl::kvs> kvs;
    ccl::kvs::address_type mainAddr;

    if (row_rank == 0) {
        kvs = ccl::create_main_kvs();
        mainAddr = kvs->get_address();
        MPI_Bcast((void *)mainAddr.data(), mainAddr.size(), MPI_BYTE, 0, row_comm);
    } else {
        MPI_Bcast((void *)mainAddr.data(), mainAddr.size(), MPI_BYTE, 0, row_comm);
        kvs = ccl::create_kvs(mainAddr);
    }

    pcomm = new ccl::communicator(ccl::create_communicator(row_size, row_rank, kvs));

    *world_size = pcomm->size();
    *world_rank = pcomm->rank();
printf("ccl world_size: %d, world_rank: %d, world_color: %d\n", *world_size, *world_rank, *world_color);
fflush(stdout);
#ifdef USE_SHM
    char myHostname[MPI_MAX_PROCESSOR_NAME];
    char all_hostnames[MPI_MAX_PROCESSOR_NAME * MPI_MAX_PROCESSOR_NAME];
    int hostnameLen;

    // Check ranks are on the same physical machine
    MPI_Get_processor_name(myHostname, &hostnameLen);
    MPI_Allgather(myHostname, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, all_hostnames, MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
            MPI_COMM_WORLD);

    int sameHostnames = 1;
    for (int i = 1; i < *world_size; i++) {
        if (strcmp(myHostname, &all_hostnames[i * MPI_MAX_PROCESSOR_NAME]) != 0) {
            sameHostnames = 0;
            break;
        }
    }
    return sameHostnames;
#endif
    return 0;
}

extern "C" void mpiFinalize() {
    int isFinalized = 0;
    MPI_Finalized(&isFinalized);
    if (!isFinalized) { MPI_Finalize(); }
}

extern "C" void freePCOMM() {
    delete pcomm;
}

extern "C" void allreduce(float *sendBuf, float *recvBuf, size_t count) {
    ccl::allreduce(sendBuf, recvBuf, count, ccl::reduction::sum, *pcomm).wait();
}

extern "C" void allreduceBF16(void *sendBuf, void *recvBuf, size_t count) {
    ccl::allreduce(sendBuf, recvBuf, count, ccl::datatype::bfloat16, ccl::reduction::sum, *pcomm).wait();
}

extern "C" void broadcast(int *buf, size_t count) {
    ccl::broadcast(buf, count, 0, *pcomm).wait(); // assume always broadcast from master (rank 0)
}

extern "C" void allgatherv(
        const float *sendBuf, size_t count, float *recvBuf, const std::vector<long unsigned int> &recvCounts) {
    ccl::allgatherv(sendBuf, count, recvBuf, recvCounts, *pcomm).wait();
}

extern "C" void barrier(
        const float *sendBuf, size_t count, float *recvBuf, const std::vector<long unsigned int> &recvCounts) {
    ccl::allgatherv(sendBuf, count, recvBuf, recvCounts, *pcomm).wait();
}