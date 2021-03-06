/*
 * RestoreLoader.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This file declares the actors used by the RestoreLoader role

#pragma once
#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_RESTORE_LOADER_G_H)
#define FDBSERVER_RESTORE_LOADER_G_H
#include "fdbserver/RestoreLoader.actor.g.h"
#elif !defined(FDBSERVER_RESTORE_LOADER_H)
#define FDBSERVER_RESTORE_LOADER_H

#include <sstream>
#include "flow/Stats.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/CommitTransaction.h"
#include "fdbrpc/fdbrpc.h"
#include "fdbserver/CoordinationInterface.h"
#include "fdbrpc/Locality.h"
#include "fdbclient/RestoreWorkerInterface.actor.h"
#include "fdbserver/RestoreUtil.h"
#include "fdbserver/RestoreCommon.actor.h"
#include "fdbserver/RestoreRoleCommon.actor.h"
#include "fdbclient/BackupContainer.h"

#include "flow/actorcompiler.h" // has to be last include

struct LoaderBatchData : public ReferenceCounted<LoaderBatchData> {
	std::map<LoadingParam, Future<Void>> processedFileParams;
	std::map<LoadingParam, VersionedMutationsMap> kvOpsPerLP; // Buffered kvOps for each loading param

	// rangeToApplier is in master and loader. Loader uses this to determine which applier a mutation should be sent
	//   Key is the inclusive lower bound of the key range the applier (UID) is responsible for
	std::map<Key, UID> rangeToApplier;

	// Sampled mutations to be sent back to restore master
	std::map<LoadingParam, MutationsVec> sampleMutations;
	int numSampledMutations; // The total number of mutations received from sampled data.

	Future<Void> pollMetrics;

	// Status counters
	struct Counters {
		CounterCollection cc;
		Counter loadedRangeBytes, loadedLogBytes, sentBytes;
		Counter sampledRangeBytes, sampledLogBytes;

		Counters(LoaderBatchData* self, UID loaderInterfID, int batchIndex)
		  : cc("LoaderBatch", loaderInterfID.toString() + ":" + std::to_string(batchIndex)),
		    loadedRangeBytes("LoadedRangeBytes", cc), loadedLogBytes("LoadedLogBytes", cc), sentBytes("SentBytes", cc),
		    sampledRangeBytes("SampledRangeBytes", cc), sampledLogBytes("SampledLogBytes", cc) {}
	} counters;

	explicit LoaderBatchData(UID nodeID, int batchIndex) : counters(this, nodeID, batchIndex) {
		pollMetrics =
		    traceCounters("FastRestoreLoaderMetrics", nodeID, SERVER_KNOBS->FASTRESTORE_ROLE_LOGGING_DELAY,
		                  &counters.cc, nodeID.toString() + "/RestoreLoaderMetrics/" + std::to_string(batchIndex));
		TraceEvent("FastRestoreLoaderMetricsCreated").detail("Node", nodeID);
	}

	void reset() {
		processedFileParams.clear();
		kvOpsPerLP.clear();
		sampleMutations.clear();
		numSampledMutations = 0;
		rangeToApplier.clear();
	}
};

using LoaderCounters = LoaderBatchData::Counters;

struct LoaderBatchStatus : public ReferenceCounted<LoaderBatchStatus> {
	Optional<Future<Void>> sendAllRanges;
	Optional<Future<Void>> sendAllLogs;

	void addref() { return ReferenceCounted<LoaderBatchStatus>::addref(); }
	void delref() { return ReferenceCounted<LoaderBatchStatus>::delref(); }

	std::string toString() {
		std::stringstream ss;
		ss << "sendAllRanges: "
		   << (!sendAllRanges.present() ? "invalid" : (sendAllRanges.get().isReady() ? "ready" : "notReady"))
		   << " sendAllLogs: "
		   << (!sendAllLogs.present() ? "invalid" : (sendAllLogs.get().isReady() ? "ready" : "notReady"));
		return ss.str();
	}
};

struct RestoreLoaderData : RestoreRoleData, public ReferenceCounted<RestoreLoaderData> {
	// buffered data per version batch
	std::map<int, Reference<LoaderBatchData>> batch;
	std::map<int, Reference<LoaderBatchStatus>> status;

	Reference<IBackupContainer> bc; // Backup container is used to read backup files
	Key bcUrl; // The url used to get the bc

	void addref() { return ReferenceCounted<RestoreLoaderData>::addref(); }
	void delref() { return ReferenceCounted<RestoreLoaderData>::delref(); }

	explicit RestoreLoaderData(UID loaderInterfID, int assignedIndex) {
		nodeID = loaderInterfID;
		nodeIndex = assignedIndex;
		role = RestoreRole::Loader;
	}

	~RestoreLoaderData() = default;

	std::string describeNode() {
		std::stringstream ss;
		ss << "[Role: Loader] [NodeID:" << nodeID.toString().c_str() << "] [NodeIndex:" << std::to_string(nodeIndex)
		   << "]";
		return ss.str();
	}

	void initVersionBatch(int batchIndex) {
		TraceEvent("FastRestore").detail("InitVersionBatchOnLoader", nodeID);
		batch[batchIndex] = Reference<LoaderBatchData>(new LoaderBatchData(nodeID, batchIndex));
		status[batchIndex] = Reference<LoaderBatchStatus>(new LoaderBatchStatus());
	}

	void resetPerRestoreRequest() {
		batch.clear();
		status.clear();
	}

	void initBackupContainer(Key url) {
		if (bcUrl == url && bc.isValid()) {
			return;
		}
		bcUrl = url;
		bc = IBackupContainer::openContainer(url.toString());
	}
};

ACTOR Future<Void> restoreLoaderCore(RestoreLoaderInterface loaderInterf, int nodeIndex, Database cx);

#include "flow/unactorcompiler.h"
#endif
