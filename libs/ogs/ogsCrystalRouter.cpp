/*

The MIT License (MIT)

Copyright (c) 2017-2021 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "ogs.hpp"
#include "ogs/ogsUtils.hpp"
#include "ogs/ogsExchange.hpp"

namespace libp {

namespace ogs {

void ogsCrystalRouter_t::Start(const int k,
                               const Type type,
                               const Op op,
                               const Transpose trans,
                               const bool host){

  occa::device &device = platform.device;

  //get current stream
  occa::stream currentStream = device.getStream();

  const dlong N = (trans == NoTrans) ? NhaloP : Nhalo;

  if (N) {
    if (!gpu_aware && !host) {
      //if not using gpu-aware mpi and exchanging device buffers,
      // move the halo buffer to the host
      device.setStream(dataStream);
      const size_t Nbytes = k*Sizeof(type);
      o_haloBuf.copyTo(haloBuf, N*Nbytes, 0, "async: true");
      device.setStream(currentStream);
    }
  }
}


void ogsCrystalRouter_t::Finish(const int k,
                                const Type type,
                                const Op op,
                                const Transpose trans,
                                const bool host){

  const size_t Nbytes = k*Sizeof(type);
  occa::device &device = platform.device;

  //get current stream
  occa::stream currentStream = device.getStream();

  //the intermediate kernels are always overlapped with the default stream
  device.setStream(dataStream);

  dlong N = (trans == NoTrans) ? NhaloP : Nhalo;

  if (N && !gpu_aware && !host) {
    //synchronize data stream to ensure the host buffer is on the host
    device.finish();
  }

  libp::memory<crLevel> levels;
  if (trans==NoTrans)
    levels = levelsN;
  else
    levels = levelsT;

  for (int l=0;l<Nlevels;l++) {

    char *sendPtr, *recvPtr;
    if (gpu_aware && !host) { //device pointer
      sendPtr = (char*)o_sendBuf.ptr();
      recvPtr = (char*)o_haloBuf.ptr() + levels[l].recvOffset*Nbytes;
    } else { //host pointer
      sendPtr = sendBuf;
      recvPtr = haloBuf + levels[l].recvOffset*Nbytes;
    }

    //post recvs
    if (levels[l].Nmsg>0) {
      MPI_Irecv(recvPtr, k*levels[l].Nrecv0, MPI_Type(type),
                levels[l].partner, levels[l].partner, comm, request+1);
    }
    if (levels[l].Nmsg==2) {
      MPI_Irecv(recvPtr+levels[l].Nrecv0*Nbytes,
                k*levels[l].Nrecv1, MPI_Type(type),
                rank-1, rank-1, comm, request+2);
    }

    //assemble send buffer
    if (gpu_aware) {
      if (levels[l].Nsend) {
        extractKernel[type](levels[l].Nsend, k,
                            levels[l].o_sendIds,
                            o_haloBuf, o_sendBuf);
        device.finish();
      }
    } else {
      extract(levels[l].Nsend, k, type,
              levels[l].sendIds.ptr(), haloBuf, sendBuf);
    }

    //post send
    MPI_Isend(sendPtr, k*levels[l].Nsend, MPI_Type(type),
              levels[l].partner, rank, comm, request+0);
    MPI_Waitall(levels[l].Nmsg+1, request, status);

    //rotate buffers
    o_recvBuf = o_buf[(buf_id+0)%2];
    o_haloBuf = o_buf[(buf_id+1)%2];
    recvBuf = buf[(buf_id+0)%2];
    haloBuf = buf[(buf_id+1)%2];
    buf_id = (buf_id+1)%2;

    //Gather the recv'd values into the haloBuffer
    if (gpu_aware) {
      levels[l].gather.Gather(o_haloBuf, o_recvBuf,
                               k, type, op, Trans);
    } else {
      levels[l].gather.Gather(haloBuf, recvBuf,
                               k, type, op, Trans);
    }
  }

  N = (trans == Trans) ? NhaloP : Nhalo;
  if (N) {
    if (!gpu_aware && !host) {
      // copy recv back to device
      o_haloBuf.copyFrom(haloBuf, N*Nbytes, 0, "async: true");
      device.finish(); //wait for transfer to finish
    }
  }

  device.setStream(currentStream);
}

/*
 *Crystal Router performs the needed MPI communcation via recursive
 * folding of a hypercube. Consider a set of NP ranks. We select a
 * pivot point n_half=(NP+1)/2, and pair all ranks r<n_half (called
 * lo half) the with ranks r>=n_half (called the hi half), as follows
 *
 *                0 <--> NP-1
 *                1 <--> NP-2
 *                2 <--> NP-3
 *                  * * *
 *         n_half-2 <--> NP-n_half+1
 *         n_half-1 <--> NP-n_half
 *
 * The communication can then be summarized thusly: if a rank in the lo
 * half has data needed by *any* rank in the hi half, it sends this data
 * to its hi partner, and analogously for ranks in the hi half. Each rank
 * therefore sends/receives a single message to/from its partner.
 *
 * The communication then proceeds recursively, applying the same folding
 * proceedure to the lo and hi halves seperately, and stopping when the size
 * of the local NP reaches 1.
 *
 * In the case where NP is odd, n_half-1 == NP-n_half and rank n_half-1 has
 * no partner to communicate with. In this case, we assign rank r to the
 * lo half of ranks, and rank n_half-1 sends its data to rank n_half (and
 * receives no message, as rank n_half-2 is receiving all rank n_half's data).

 * To perform the Crystal Router exchange, each rank gathers its halo nodes to
 * a coalesced buffer. At each step in the crystal router, a send buffer is
 * gathered from this buffer and sent to the rank's partner. Simultaneously, a
 * buffer is received from the rank's partner. This receive buffer is scattered
 * and added into the coalesced halo buffer. After all commincation is complete
 * the halo nodes are scattered back to the output array.
 */

ogsCrystalRouter_t::ogsCrystalRouter_t(dlong Nshared,
                                       parallelNode_t sharedNodes[],
                                       ogsOperator_t& gatherHalo,
                                       MPI_Comm _comm,
                                       platform_t &_platform):
  ogsExchange_t(_platform,_comm) {

  NhaloP = gatherHalo.NrowsN;
  Nhalo  = gatherHalo.NrowsT;

  //first count how many levels we need
  Nlevels = 0;
  int np = size;
  int np_offset=0;
  while (np>1) {
    int np_half = (np+1)/2;
    int r_half = np_half + np_offset;

    int is_lo = (rank<r_half) ? 1 : 0;

    //Shrink the size of the hypercube
    if (is_lo) {
      np = np_half;
    } else {
      np -= np_half;
      np_offset = r_half;
    }
    Nlevels++;
  }
  levelsN.malloc(Nlevels);
  levelsT.malloc(Nlevels);


  //Now build the levels
  Nlevels = 0;
  np = size;
  np_offset=0;

  dlong N = Nshared + Nhalo;
  parallelNode_t* nodes = new parallelNode_t[N];

  //setup is easier if we include copies of the nodes we own
  // in the list of shared nodes
  for(dlong n=0;n<Nhalo;++n) {
    nodes[n].newId = n;
    nodes[n].sign  = (n<NhaloP) ? 2 : -2;
    nodes[n].baseId = 0;
    nodes[n].rank = rank;
  }
  for(dlong n=0;n<Nshared;++n) {
    const dlong newId = sharedNodes[n].newId;
    if (nodes[newId].baseId==0) {
      if (newId<NhaloP)
        nodes[newId].baseId = abs(sharedNodes[n].baseId);
      else
        nodes[newId].baseId = -abs(sharedNodes[n].baseId);
    }
  }
  for(dlong n=Nhalo;n<N;++n) nodes[n] = sharedNodes[n-Nhalo];

  std::sort(nodes, nodes+N,
            [](const parallelNode_t& a, const parallelNode_t& b) {
              return a.newId < b.newId; //group by newId (which also groups by abs(baseId))
            });

  dlong haloBuf_size = Nhalo;

  dlong NhaloExtT = Nhalo;
  dlong NhaloExtN = Nhalo;

  while (np>1) {
    int np_half = (np+1)/2;
    int r_half = np_half + np_offset;

    int is_lo = (rank<r_half) ? 1 : 0;

    int partner = np-1-(rank-np_offset)+np_offset;
    int Nmsg=1;
    if (partner==rank) {
      partner=r_half;
      Nmsg=0;
    }
    if (np&1 && rank==r_half) {
      Nmsg=2;
    }
    levelsN[Nlevels].partner = partner;
    levelsT[Nlevels].partner = partner;
    levelsN[Nlevels].Nmsg = Nmsg;
    levelsT[Nlevels].Nmsg = Nmsg;

    //count lo/hi nodes
    dlong Nlo=0, Nhi=0;
    for (dlong n=0;n<N;n++) {
      if (nodes[n].rank<r_half)
        Nlo++;
      else
        Nhi++;
    }

    int Nsend=(is_lo) ? Nhi : Nlo;

    MPI_Isend(&Nsend, 1, MPI_INT, partner, rank, comm, request+0);

    int Nrecv0=0, Nrecv1=0;
    if (Nmsg>0)
      MPI_Irecv(&Nrecv0, 1, MPI_INT, partner, partner, comm, request+1);
    if (Nmsg==2)
      MPI_Irecv(&Nrecv1, 1, MPI_INT, r_half-1, r_half-1, comm, request+2);

    MPI_Waitall(Nmsg+1, request, status);

    int Nrecv = Nrecv0+Nrecv1;

    //make room for the nodes we'll recv
    if (is_lo) Nlo+=Nrecv;
    else       Nhi+=Nrecv;

    //split node list in two
    parallelNode_t *loNodes = new parallelNode_t[Nlo];
    parallelNode_t *hiNodes = new parallelNode_t[Nhi];

    Nlo=0, Nhi=0;
    for (dlong n=0;n<N;n++) {
      if (nodes[n].rank<r_half)
        loNodes[Nlo++] = nodes[n];
      else
        hiNodes[Nhi++] = nodes[n];
    }

    //free up space
    delete[] nodes;

    //point to the buffer we keep after the comms
    nodes = is_lo ? loNodes : hiNodes;
    N     = is_lo ? Nlo+Nrecv : Nhi+Nrecv;

    const int offset = is_lo ? Nlo : Nhi;
    parallelNode_t *sendNodes = is_lo ? hiNodes : loNodes;

    //count how many entries from the halo buffer we're sending
    int NentriesSendN=0;
    int NentriesSendT=0;
    for (dlong n=0;n<Nsend;n++) {
      if (n==0 || abs(sendNodes[n].baseId)!=abs(sendNodes[n-1].baseId)) {
        if (sendNodes[n].sign>0) NentriesSendN++;
        NentriesSendT++;
      }
    }
    levelsN[Nlevels].Nsend = NentriesSendN;
    levelsT[Nlevels].Nsend = NentriesSendT;
    levelsN[Nlevels].sendIds.malloc(NentriesSendN);
    levelsT[Nlevels].sendIds.malloc(NentriesSendT);

    NentriesSendN=0; //reset
    NentriesSendT=0; //reset
    for (dlong n=0;n<Nsend;n++) {
      if (n==0 || abs(sendNodes[n].baseId)!=abs(sendNodes[n-1].baseId)) {
        if (sendNodes[n].sign>0)
          levelsN[Nlevels].sendIds[NentriesSendN++] = sendNodes[n].newId;

        levelsT[Nlevels].sendIds[NentriesSendT++] = sendNodes[n].newId;
      }
      sendNodes[n].newId = -1; //wipe the newId before sending
    }
    levelsT[Nlevels].o_sendIds = platform.malloc(NentriesSendT*sizeof(dlong),
                                                levelsT[Nlevels].sendIds.ptr());
    levelsN[Nlevels].o_sendIds = platform.malloc(NentriesSendN*sizeof(dlong),
                                                levelsN[Nlevels].sendIds.ptr());

    //share the entry count with our partner
    MPI_Isend(&NentriesSendT, 1, MPI_INT, partner, rank, comm, request+0);

    int NentriesRecvT0=0, NentriesRecvT1=0;
    if (Nmsg>0)
      MPI_Irecv(&NentriesRecvT0, 1, MPI_INT, partner, partner, comm, request+1);
    if (Nmsg==2)
      MPI_Irecv(&NentriesRecvT1, 1, MPI_INT, r_half-1, r_half-1, comm, request+2);

    MPI_Waitall(Nmsg+1, request, status);

    levelsT[Nlevels].Nrecv0 = NentriesRecvT0;
    levelsT[Nlevels].Nrecv1 = NentriesRecvT1;
    levelsT[Nlevels].recvOffset = NhaloExtT;

    MPI_Isend(&NentriesSendN, 1, MPI_INT, partner, rank, comm, request+0);

    int NentriesRecvN0=0, NentriesRecvN1=0;
    if (Nmsg>0)
      MPI_Irecv(&NentriesRecvN0, 1, MPI_INT, partner, partner, comm, request+1);
    if (Nmsg==2)
      MPI_Irecv(&NentriesRecvN1, 1, MPI_INT, r_half-1, r_half-1, comm, request+2);

    MPI_Waitall(Nmsg+1, request, status);

    levelsN[Nlevels].Nrecv0 = NentriesRecvN0;
    levelsN[Nlevels].Nrecv1 = NentriesRecvN1;
    levelsN[Nlevels].recvOffset = NhaloExtN;

    //space needed in recv buffer for this level
    dlong buf_size = NhaloExtT + NentriesRecvT0 + NentriesRecvT1;
    haloBuf_size = (buf_size > haloBuf_size) ? buf_size : haloBuf_size;


    //send half the list to our partner
    MPI_Isend(sendNodes, Nsend,
              MPI_PARALLELNODE_T, partner, rank, comm, request+0);

    //recv new nodes from our partner(s)
    if (Nmsg>0)
      MPI_Irecv(nodes+offset,        Nrecv0,
                MPI_PARALLELNODE_T, partner, partner, comm, request+1);
    if (Nmsg==2)
      MPI_Irecv(nodes+offset+Nrecv0, Nrecv1,
                MPI_PARALLELNODE_T, r_half-1, r_half-1, comm, request+2);

    MPI_Waitall(Nmsg+1, request, status);

    delete[] sendNodes;

    //We now have a list of nodes who's destinations are in our half
    // of the hypercube
    //We now build the gather into the haloBuffer


    //record the current order
    for (dlong n=0;n<N;n++) nodes[n].localId = n;

    //sort the new node list by baseId to find matches
    std::sort(nodes, nodes+N,
            [](const parallelNode_t& a, const parallelNode_t& b) {
              if(abs(a.baseId) < abs(b.baseId)) return true; //group by abs(baseId)
              if(abs(a.baseId) > abs(b.baseId)) return false;

              return a.newId > b.newId; //positive newIds first
            });

    //find how many positive ids there will be in the extended halo
    dlong start = 0;
    NhaloExtN=0;
    NhaloExtT=0;
    for (dlong n=0;n<N;++n) {
      //for each baseId group
      if (n==N-1 || (abs(nodes[n].baseId)!=abs(nodes[n+1].baseId))) {
        dlong end = n+1;
        const dlong id = nodes[start].newId; //get Id

        //if this id is in the extended halo already,
        // or if it is a new baseId to arrive, look for
        // a positive node
        if (id >= Nhalo || id==-1) {
          for (dlong i=start;i<end;++i) {
            if (nodes[i].sign>0) {
              NhaloExtN++;
              break;
            }
          }
          NhaloExtT++;
        }
        start = end;
      }
    }


    //make an index map to save the original extended halo ids
    dlong *indexMap = new dlong[NhaloExtT];

    //fill newIds of new entries if possible, or give them an index
    NhaloExtT = Nhalo + NhaloExtN;
    NhaloExtN = Nhalo;
    start = 0;
    for (dlong n=0;n<N;++n) {
      //for each baseId group
      if (n==N-1 || (abs(nodes[n].baseId)!=abs(nodes[n+1].baseId))) {
        dlong end = n+1;

        dlong id = nodes[start].newId; //get Id

        //if this id is in the extended halo already,
        // or if it is a new baseId to arrive, give it
        // a new id in the extended halo
        if (id >= Nhalo || id==-1) {
          int sign = -2;
          for (dlong i=start;i<end;++i) {
            if (nodes[i].sign>0) {
              sign = nodes[i].sign;
              break;
            }
          }

          if (sign>0)
            id = NhaloExtN++;
          else
            id = NhaloExtT++;

          //save the orignal id
          indexMap[id-Nhalo] = nodes[start].newId;
        }

        //write id into this baseId group
        for (dlong i=start;i<end;++i)
          nodes[i].newId = id;

        start = end;
      }
    }

    //sort back to first ordering
    permute(N, nodes, [](const parallelNode_t& a) { return a.localId; } );

    ogsOperator_t gatherN(platform);
    ogsOperator_t gatherT(platform);

    gatherN.kind = Unsigned;
    gatherT.kind = Unsigned;

    gatherN.NrowsN = NhaloExtN;
    gatherN.NrowsT = NhaloExtN;
    gatherN.Ncols  = levelsN[Nlevels].recvOffset
                      + NentriesRecvN0 + NentriesRecvN1;

    gatherT.NrowsN = NhaloExtT;
    gatherT.NrowsT = NhaloExtT;
    gatherT.Ncols  = levelsT[Nlevels].recvOffset
                      + NentriesRecvT0 + NentriesRecvT1;

    gatherT.rowStartsT.calloc(gatherT.NrowsT+1);
    gatherT.rowStartsN = gatherT.rowStartsT;

    gatherN.rowStartsT.calloc(gatherT.NrowsT+1);
    gatherN.rowStartsN = gatherN.rowStartsT;

    //gatherT the existing halo
    for (dlong n=0;n<Nhalo;++n) gatherT.rowStartsT[n+1]=1;

    //for notrans theres nothing to gather in the negative nodes the first time
    if (np==size)
      for (dlong n=0;n<NhaloP;++n) gatherN.rowStartsT[n+1]=1;
    else
      for (dlong n=0;n<Nhalo;++n) gatherN.rowStartsT[n+1]=1;

    //look through the nodes we still have for extended halo nodes
    for (dlong n=0;n<offset;++n) {
      if (n==0 || abs(nodes[n].baseId)!=abs(nodes[n-1].baseId)) {
        const dlong id = nodes[n].newId;
        if (nodes[n].newId >= Nhalo) {
          if (nodes[n].sign >0) gatherN.rowStartsT[id+1]++;
          gatherT.rowStartsT[id+1]++;
        }
      }
    }

    //look through first message for nodes to gather
    for (dlong n=offset;n<offset+Nrecv0;++n) {
      if (n==offset || abs(nodes[n].baseId)!=abs(nodes[n-1].baseId)) {
        const dlong id = nodes[n].newId;
        if (nodes[n].sign >0) gatherN.rowStartsT[id+1]++;
        gatherT.rowStartsT[id+1]++;
      }
    }
    //look through second message for nodes to gather
    for (dlong n=offset+Nrecv0;n<N;++n) {
      if (n==offset+Nrecv0 || abs(nodes[n].baseId)!=abs(nodes[n-1].baseId)) {
        const dlong id = nodes[n].newId;
        if (nodes[n].sign >0) gatherN.rowStartsT[id+1]++;
        gatherT.rowStartsT[id+1]++;
      }
    }

    for (dlong i=0;i<gatherT.NrowsT;i++) {
      gatherT.rowStartsT[i+1] += gatherT.rowStartsT[i];
      gatherN.rowStartsT[i+1] += gatherN.rowStartsT[i];
    }

    gatherT.nnzT = gatherT.rowStartsT[gatherT.NrowsT];
    gatherT.nnzN = gatherT.rowStartsT[gatherT.NrowsT];

    gatherT.colIdsT.calloc(gatherT.nnzT);
    gatherT.colIdsN = gatherT.colIdsT;

    gatherN.nnzT = gatherN.rowStartsT[gatherN.NrowsT];
    gatherN.nnzN = gatherN.rowStartsT[gatherN.NrowsT];

    gatherN.colIdsT.calloc(gatherN.nnzT);
    gatherN.colIdsN = gatherN.colIdsT;

    //gatherT the existing halo
    for (dlong n=0;n<Nhalo;++n) {
      gatherT.colIdsT[gatherT.rowStartsT[n]++] = n;
    }

    if (np==size) {
      for (dlong n=0;n<NhaloP;++n) {
        gatherN.colIdsT[gatherN.rowStartsT[n]++] = n;
      }
    } else {
      for (dlong n=0;n<Nhalo;++n) {
        gatherN.colIdsT[gatherN.rowStartsT[n]++] = n;
      }
    }

    //look through the nodes we still have for extended halo nodes
    for (dlong n=0;n<offset;++n) {
      if (n==0 || abs(nodes[n].baseId)!=abs(nodes[n-1].baseId)) {
        const dlong id = nodes[n].newId;
        if (nodes[n].newId >= Nhalo) {
          if (nodes[n].sign > 0) {
            gatherN.colIdsT[gatherN.rowStartsT[id]++] = indexMap[id-Nhalo];
          }
          gatherT.colIdsT[gatherT.rowStartsT[id]++] = indexMap[id-Nhalo];
        }
      }
    }

    delete[] indexMap;

    dlong NentriesRecvN=levelsN[Nlevels].recvOffset;
    dlong NentriesRecvT=levelsT[Nlevels].recvOffset;
    //look through first message for nodes to gatherT
    for (dlong n=offset;n<offset+Nrecv0;++n) {
      if (n==offset || abs(nodes[n].baseId)!=abs(nodes[n-1].baseId)) {
        const dlong id = nodes[n].newId;
        if (nodes[n].sign > 0) {
          gatherN.colIdsT[gatherN.rowStartsT[id]++] = NentriesRecvN++;
        }
        gatherT.colIdsT[gatherT.rowStartsT[id]++] = NentriesRecvT++;
      }
    }
    //look through second message for nodes to gatherT
    for (dlong n=offset+Nrecv0;n<N;++n) {
      if (n==offset+Nrecv0 || abs(nodes[n].baseId)!=abs(nodes[n-1].baseId)) {
        const dlong id = nodes[n].newId;
        if (nodes[n].sign > 0) {
          gatherN.colIdsT[gatherN.rowStartsT[id]++] = NentriesRecvN++;
        }
        gatherT.colIdsT[gatherT.rowStartsT[id]++] = NentriesRecvT++;
      }
    }

    //reset row starts
    for (dlong i=gatherT.NrowsT;i>0;--i) {
      gatherT.rowStartsT[i] = gatherT.rowStartsT[i-1];
      gatherN.rowStartsT[i] = gatherN.rowStartsT[i-1];
    }
    gatherT.rowStartsT[0] = 0;
    gatherN.rowStartsT[0] = 0;

    gatherT.o_rowStartsT = platform.malloc((gatherT.NrowsT+1)*sizeof(dlong), gatherT.rowStartsT.ptr());
    gatherT.o_rowStartsN = gatherT.o_rowStartsT;
    gatherN.o_rowStartsT = platform.malloc((gatherN.NrowsT+1)*sizeof(dlong), gatherN.rowStartsT.ptr());
    gatherN.o_rowStartsN = gatherN.o_rowStartsT;
    gatherT.o_colIdsT = platform.malloc((gatherT.nnzT)*sizeof(dlong), gatherT.colIdsT.ptr());
    gatherT.o_colIdsN = gatherT.o_colIdsT;
    gatherN.o_colIdsT = platform.malloc((gatherN.nnzT)*sizeof(dlong), gatherN.colIdsT.ptr());
    gatherN.o_colIdsN = gatherN.o_colIdsT;

    gatherN.setupRowBlocks();
    gatherT.setupRowBlocks();

    levelsT[Nlevels].gather = gatherT;
    levelsN[Nlevels].gather = gatherN;

    //sort the new node list by newId
    std::sort(nodes, nodes+N,
            [](const parallelNode_t& a, const parallelNode_t& b) {
              return a.newId < b.newId; //group by newId (which also groups by abs(baseId))
            });

    //propagate the sign of recvieved nodes
    start = 0;
    for (dlong n=0;n<N;++n) {
      //for each baseId group
      if (n==N-1 || (abs(nodes[n].baseId)!=abs(nodes[n+1].baseId))) {
        dlong end = n+1;
        //look for a positive sign, so we know if this node flips positive
        for (dlong i=start;i<end;++i) {
          const int sign = nodes[i].sign;
          if (sign>0) {
            for (dlong j=start;j<end;++j)
              nodes[j].sign = sign;
            break;
          }
        }
        start = end;
      }
    }

    //Shrink the size of the hypercube
    if (is_lo) {
      np = np_half;
    } else {
      np -= np_half;
      np_offset = r_half;
    }
    Nlevels++;
  }
  if (size>1) delete[] nodes;

  NsendMax=0, NrecvMax=0;
  for (int k=0;k<Nlevels;k++) {
    int Nsend = levelsT[k].Nsend;
    NsendMax = (Nsend>NsendMax) ? Nsend : NsendMax;
    int Nrecv = levelsT[k].recvOffset
                + levelsT[k].Nrecv0 + levelsT[k].Nrecv1;
    NrecvMax = (Nrecv>NrecvMax) ? Nrecv : NrecvMax;
  }

  //make scratch space
  AllocBuffer(Sizeof(Dfloat));
}

void ogsCrystalRouter_t::AllocBuffer(size_t Nbytes) {

  if (o_sendBuf.size() < NsendMax*Nbytes) {
    sendBuf = static_cast<char*>(platform.hostMalloc(NsendMax*Nbytes,  nullptr, h_sendBuf));
    o_sendBuf = platform.malloc(NsendMax*Nbytes);
  }
  if (o_buf[0].size() < NrecvMax*Nbytes) {
    buf[0] = static_cast<char*>(platform.hostMalloc(NrecvMax*Nbytes,  nullptr, h_buf[0]));
    buf[1] = static_cast<char*>(platform.hostMalloc(NrecvMax*Nbytes,  nullptr, h_buf[1]));
    haloBuf = buf[0];
    recvBuf = buf[1];

    o_buf[0] = platform.malloc(NrecvMax*Nbytes);
    o_buf[1] = platform.malloc(NrecvMax*Nbytes);
    o_haloBuf = o_buf[0];
    o_recvBuf = o_buf[1];
    buf_id=0;
  }
}

} //namespace ogs

} //namespace libp
