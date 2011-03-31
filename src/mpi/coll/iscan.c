/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2010 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "mpiimpl.h"
#include "collutil.h"

/* -- Begin Profiling Symbol Block for routine MPIX_Iscan */
#if defined(HAVE_PRAGMA_WEAK)
#pragma weak MPIX_Iscan = PMPIX_Iscan
#elif defined(HAVE_PRAGMA_HP_SEC_DEF)
#pragma _HP_SECONDARY_DEF PMPIX_Iscan  MPIX_Iscan
#elif defined(HAVE_PRAGMA_CRI_DUP)
#pragma _CRI duplicate MPIX_Iscan as PMPIX_Iscan
#endif
/* -- End Profiling Symbol Block */

/* Define MPICH_MPI_FROM_PMPI if weak symbols are not supported to build
   the MPI routines */
#ifndef MPICH_MPI_FROM_PMPI
#undef MPIX_Iscan
#define MPIX_Iscan PMPIX_Iscan

/* any non-MPI functions go here, especially non-static ones */

/* This is the default implementation of scan. The algorithm is:

   Algorithm: MPI_Scan

   We use a lgp recursive doubling algorithm. The basic algorithm is
   given below. (You can replace "+" with any other scan operator.)
   The result is stored in recvbuf.

 .vb
   recvbuf = sendbuf;
   partial_scan = sendbuf;
   mask = 0x1;
   while (mask < size) {
      dst = rank^mask;
      if (dst < size) {
         send partial_scan to dst;
         recv from dst into tmp_buf;
         if (rank > dst) {
            partial_scan = tmp_buf + partial_scan;
            recvbuf = tmp_buf + recvbuf;
         }
         else {
            if (op is commutative)
               partial_scan = tmp_buf + partial_scan;
            else {
               tmp_buf = partial_scan + tmp_buf;
               partial_scan = tmp_buf;
            }
         }
      }
      mask <<= 1;
   }
 .ve

   End Algorithm: MPI_Scan
*/
#undef FUNCNAME
#define FUNCNAME MPIR_Iscan_rec_dbl
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPIR_Iscan_rec_dbl(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;
    MPI_Aint true_extent, true_lb, extent;
    int is_commutative;
    int mask, dst, rank, comm_size;
    void *partial_scan = NULL;
    void *tmp_buf = NULL;
    MPIR_SCHED_CHKPMEM_DECL(2);

    if (count == 0)
        goto fn_exit;

    comm_size = comm_ptr->local_size;
    rank = comm_ptr->rank;

    is_commutative = MPIR_Op_is_commutative(op);

    /* need to allocate temporary buffer to store partial scan*/
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);

    MPID_Datatype_get_extent_macro(datatype, extent);
    MPIR_SCHED_CHKPMEM_MALLOC(partial_scan, void *, count*(MPIR_MAX(extent,true_extent)), mpi_errno, "partial_scan");

    /* This eventually gets malloc()ed as a temp buffer, not added to
     * any user buffers */
    MPID_Ensure_Aint_fits_in_pointer(count * MPIR_MAX(extent, true_extent));

    /* adjust for potential negative lower bound in datatype */
    partial_scan = (void *)((char*)partial_scan - true_lb);

    /* need to allocate temporary buffer to store incoming data*/
    MPIR_SCHED_CHKPMEM_MALLOC(tmp_buf, void *, count*(MPIR_MAX(extent,true_extent)), mpi_errno, "tmp_buf");

    /* adjust for potential negative lower bound in datatype */
    tmp_buf = (void *)((char*)tmp_buf - true_lb);

    /* Since this is an inclusive scan, copy local contribution into
       recvbuf. */
    if (sendbuf != MPI_IN_PLACE) {
        mpi_errno = MPID_Sched_copy(sendbuf, count, datatype,
                                    recvbuf, count, datatype, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    }

    if (sendbuf != MPI_IN_PLACE)
        mpi_errno = MPID_Sched_copy(sendbuf, count, datatype,
                                    partial_scan, count, datatype, s);
    else
        mpi_errno = MPID_Sched_copy(recvbuf, count, datatype,
                                    partial_scan, count, datatype, s);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    mask = 0x1;
    while (mask < comm_size) {
        dst = rank ^ mask;
        if (dst < comm_size) {
            /* Send partial_scan to dst. Recv into tmp_buf */
            mpi_errno = MPID_Sched_send(partial_scan, count, datatype, dst, comm_ptr, s);
            if (mpi_errno) MPIU_ERR_POP(mpi_errno);
            /* sendrecv, no barrier here */
            mpi_errno = MPID_Sched_recv(tmp_buf, count, datatype, dst, comm_ptr, s);
            if (mpi_errno) MPIU_ERR_POP(mpi_errno);
            MPID_SCHED_BARRIER(s);

            if (rank > dst) {
                mpi_errno = MPID_Sched_reduce(tmp_buf, partial_scan, count, datatype, op, s);
                if (mpi_errno) MPIU_ERR_POP(mpi_errno);
                mpi_errno = MPID_Sched_reduce(tmp_buf, recvbuf, count, datatype, op, s);
                if (mpi_errno) MPIU_ERR_POP(mpi_errno);
                MPID_SCHED_BARRIER(s);
            }
            else {
                if (is_commutative) {
                    mpi_errno = MPID_Sched_reduce(tmp_buf, partial_scan, count, datatype, op, s);
                    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
                    MPID_SCHED_BARRIER(s);
                }
                else {
                    mpi_errno = MPID_Sched_reduce(partial_scan, tmp_buf, count, datatype, op, s);
                    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
                    MPID_SCHED_BARRIER(s);

                    mpi_errno = MPID_Sched_copy(tmp_buf, count, datatype,
                                                partial_scan, count, datatype, s);
                    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
                    MPID_SCHED_BARRIER(s);
                }
            }
        }
        mask <<= 1;
    }

    MPIR_SCHED_CHKPMEM_COMMIT(s);
fn_exit:
    return mpi_errno;
fn_fail:
    MPIR_SCHED_CHKPMEM_REAP(s);
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIR_Iscan_rec_dbl
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPIR_Iscan_SMP(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;
    int rank = comm_ptr->rank;
    MPID_Comm *node_comm;
    MPID_Comm *roots_comm;
    MPI_Aint true_extent, true_lb, extent;
    void *tempbuf = NULL;
    void *prefulldata = NULL;
    void *localfulldata = NULL;
    MPIR_SCHED_CHKPMEM_DECL(3);

    /* In order to use the SMP-aware algorithm, the "op" can be
       either commutative or non-commutative, but we require a
       communicator in which all the nodes contain processes with
       consecutive ranks. */

    if (!MPIR_Comm_is_node_consecutive(comm_ptr)) {
        /* We can't use the SMP-aware algorithm, use the generic one */
        return MPIR_Iscan_rec_dbl(sendbuf, recvbuf, count, datatype, op, comm_ptr, s);
    }

    node_comm = comm_ptr->node_comm;
    roots_comm = comm_ptr->node_roots_comm;
    if (node_comm) {
        MPIU_Assert(node_comm->coll_fns && node_comm->coll_fns->Iscan && node_comm->coll_fns->Ibcast);
    }
    if (roots_comm) {
        MPIU_Assert(roots_comm->coll_fns && roots_comm->coll_fns->Iscan);
    }

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);

    MPID_Ensure_Aint_fits_in_pointer(count * MPIR_MAX(extent, true_extent));

    MPIR_SCHED_CHKPMEM_MALLOC(tempbuf, void *, count*(MPIR_MAX(extent, true_extent)),
                        mpi_errno, "temporary buffer");
    tempbuf = (void *)((char*)tempbuf - true_lb);

    /* Create prefulldata and localfulldata on local roots of all nodes */
    if (comm_ptr->node_roots_comm != NULL) {
        MPIR_SCHED_CHKPMEM_MALLOC(prefulldata, void *, count*(MPIR_MAX(extent, true_extent)),
                            mpi_errno, "prefulldata for scan");
        prefulldata = (void *)((char*)prefulldata - true_lb);

        if (node_comm != NULL) {
            MPIR_SCHED_CHKPMEM_MALLOC(localfulldata, void *, count*(MPIR_MAX(extent, true_extent)),
                                mpi_errno, "localfulldata for scan");
            localfulldata = (void *)((char*)localfulldata - true_lb);
        }
    }

    /* perform intranode scan to get temporary result in recvbuf. if there is only
       one process, just copy the raw data. */
    if (node_comm != NULL) {
        mpi_errno = node_comm->coll_fns->Iscan(sendbuf, recvbuf, count, datatype, op, node_comm, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        MPID_SCHED_BARRIER(s);
    }
    else if (sendbuf != MPI_IN_PLACE) {
        mpi_errno = MPID_Sched_copy(sendbuf, count, datatype,
                                    recvbuf, count, datatype, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        MPID_SCHED_BARRIER(s);
    }

    /* get result from local node's last processor which
       contains the reduce result of the whole node. Name it as
       localfulldata. For example, localfulldata from node 1 contains
       reduced data of rank 1,2,3. */
    if (roots_comm != NULL && node_comm != NULL) {
        mpi_errno = MPID_Sched_recv(localfulldata, count, datatype,
                                    (node_comm->local_size - 1), node_comm, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        MPID_SCHED_BARRIER(s);
    }
    else if (roots_comm == NULL && node_comm != NULL && node_comm->rank == node_comm->local_size - 1) {
        mpi_errno = MPID_Sched_send(recvbuf, count, datatype, 0, node_comm, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        MPID_SCHED_BARRIER(s);
    }
    else if (roots_comm != NULL) {
        localfulldata = recvbuf;
    }

    /* do scan on localfulldata to prefulldata. for example,
       prefulldata on rank 4 contains reduce result of ranks
       1,2,3,4,5,6. it will be sent to rank 7 which is master
       process of node 3. */
    if (roots_comm != NULL) {
        /* FIXME just use roots_comm->rank instead */
        int roots_rank = MPIU_Get_internode_rank(comm_ptr, rank);
        MPIU_Assert(roots_rank == roots_comm->rank);

        mpi_errno = roots_comm->coll_fns->Iscan(localfulldata, prefulldata, count, datatype, op, roots_comm, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        MPID_SCHED_BARRIER(s);

        if (roots_rank != roots_comm->local_size-1) {
            mpi_errno = MPID_Sched_send(prefulldata, count, datatype, (roots_rank + 1), roots_comm, s);
            if (mpi_errno) MPIU_ERR_POP(mpi_errno);
            MPID_SCHED_BARRIER(s);
        }
        if (roots_rank != 0) {
            mpi_errno = MPID_Sched_recv(tempbuf, count, datatype, (roots_rank - 1), roots_comm, s);
            if (mpi_errno) MPIU_ERR_POP(mpi_errno);
            MPID_SCHED_BARRIER(s);
        }
    }

    /* now tempbuf contains all the data needed to get the correct
       scan result. for example, to node 3, it will have reduce result
       of rank 1,2,3,4,5,6 in tempbuf.
       then we should broadcast this result in the local node, and
       reduce it with recvbuf to get final result if nessesary. */

    if (MPIU_Get_internode_rank(comm_ptr, rank) != 0) {
        /* we aren't on "node 0", so our node leader (possibly us) received
         * "prefulldata" from another leader into "tempbuf" */

        if (node_comm != NULL) {
            mpi_errno = node_comm->coll_fns->Ibcast(tempbuf, count, datatype, 0, node_comm, s);
            if (mpi_errno) MPIU_ERR_POP(mpi_errno);
            MPID_SCHED_BARRIER(s);
        }

        /* do reduce on tempbuf and recvbuf, finish scan. */
        mpi_errno = MPID_Sched_reduce(tempbuf, recvbuf, count, datatype, op, s);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    }

    MPIR_SCHED_CHKPMEM_COMMIT(s);
fn_exit:
    return mpi_errno;
fn_fail:
    MPIR_SCHED_CHKPMEM_REAP(s);
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIR_Iscan_impl
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPIR_Iscan_impl(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPID_Comm *comm_ptr, MPI_Request *request)
{
    int mpi_errno = MPI_SUCCESS;
    int tag = -1;
    MPID_Request *reqp = NULL;
    MPID_Sched_t s = MPID_SCHED_NULL;

    *request = MPI_REQUEST_NULL;

    mpi_errno = MPID_Sched_next_tag(comm_ptr, &tag);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);
    mpi_errno = MPID_Sched_create(&s);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    MPIU_Assert(comm_ptr->coll_fns != NULL);
    MPIU_Assert(comm_ptr->coll_fns->Iscan != NULL);
    mpi_errno = comm_ptr->coll_fns->Iscan(sendbuf, recvbuf, count, datatype, op, comm_ptr, s);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    mpi_errno = MPID_Sched_start(&s, comm_ptr, tag, &reqp);
    if (reqp)
        *request = reqp->handle;
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

fn_exit:
    return mpi_errno;
fn_fail:
    goto fn_exit;
}

#endif /* MPICH_MPI_FROM_PMPI */

#undef FUNCNAME
#define FUNCNAME MPIX_Iscan
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
/*@
MPIX_Iscan - XXX description here

Input Parameters:
+ sendbuf - starting address of the send buffer (choice)
. count - number of elements in input buffer (non-negative integer)
. datatype - data type of elements of input buffer (handle)
. op - operation (handle)
- comm - communicator (handle)

Output Parameters:
+ recvbuf - starting address of the receive buffer (choice)
- request - communication request (handle)

.N ThreadSafe

.N Fortran

.N Errors
@*/
int MPIX_Iscan(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Comm *comm_ptr = NULL;
    MPID_MPI_STATE_DECL(MPID_STATE_MPIX_ISCAN);

    MPIU_THREAD_CS_ENTER(ALLFUNC,);
    MPID_MPI_FUNC_ENTER(MPID_STATE_MPIX_ISCAN);

    /* Validate parameters, especially handles needing to be converted */
#   ifdef HAVE_ERROR_CHECKING
    {
        MPID_BEGIN_ERROR_CHECKS
        {
            MPIR_ERRTEST_DATATYPE(datatype, "datatype", mpi_errno);
            MPIR_ERRTEST_OP(op, mpi_errno);
            MPIR_ERRTEST_COMM(comm, mpi_errno);

            /* TODO more checks may be appropriate */
            if (mpi_errno != MPI_SUCCESS) goto fn_fail;
        }
        MPID_END_ERROR_CHECKS
    }
#   endif /* HAVE_ERROR_CHECKING */

    /* Convert MPI object handles to object pointers */
    MPID_Comm_get_ptr(comm, comm_ptr);

    /* Validate parameters and objects (post conversion) */
#   ifdef HAVE_ERROR_CHECKING
    {
        MPID_BEGIN_ERROR_CHECKS
        {
            MPIR_ERRTEST_COMM_INTRA(comm_ptr, mpi_errno);
            if (HANDLE_GET_KIND(datatype) != HANDLE_KIND_BUILTIN) {
                MPID_Datatype *datatype_ptr = NULL;
                MPID_Datatype_get_ptr(datatype, datatype_ptr);
                MPID_Datatype_valid_ptr(datatype_ptr, mpi_errno);
                MPID_Datatype_committed_ptr(datatype_ptr, mpi_errno);
            }

            if (HANDLE_GET_KIND(op) != HANDLE_KIND_BUILTIN) {
                MPID_Op *op_ptr = NULL;
                MPID_Op_get_ptr(op, op_ptr);
                MPID_Op_valid_ptr(op_ptr, mpi_errno);
            }
            else if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
                mpi_errno = ( * MPIR_Op_check_dtype_table[op%16 - 1] )(datatype);
            }

            MPID_Comm_valid_ptr(comm_ptr, mpi_errno);
            MPIR_ERRTEST_ARGNULL(request,"request", mpi_errno);
            /* TODO more checks may be appropriate (counts, in_place, buffer aliasing, etc) */
            if (mpi_errno != MPI_SUCCESS) goto fn_fail;
        }
        MPID_END_ERROR_CHECKS
    }
#   endif /* HAVE_ERROR_CHECKING */

    /* ... body of routine ...  */

    mpi_errno = MPIR_Iscan_impl(sendbuf, recvbuf, count, datatype, op, comm_ptr, request);
    if (mpi_errno) MPIU_ERR_POP(mpi_errno);

    /* ... end of body of routine ... */

fn_exit:
    MPID_MPI_FUNC_EXIT(MPID_STATE_MPIX_ISCAN);
    MPIU_THREAD_CS_EXIT(ALLFUNC,);
    return mpi_errno;

fn_fail:
    /* --BEGIN ERROR HANDLING-- */
#   ifdef HAVE_ERROR_CHECKING
    {
        mpi_errno = MPIR_Err_create_code(
            mpi_errno, MPIR_ERR_RECOVERABLE, FCNAME, __LINE__, MPI_ERR_OTHER,
            "**mpix_iscan", "**mpix_iscan %p %p %d %D %O %C %p", sendbuf, recvbuf, count, datatype, op, comm, request);
    }
#   endif
    mpi_errno = MPIR_Err_return_comm(comm_ptr, FCNAME, mpi_errno);
    goto fn_exit;
    /* --END ERROR HANDLING-- */
    goto fn_exit;
}
