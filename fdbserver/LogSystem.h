/*
 * LogSystem.h
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

#ifndef FDBSERVER_LOGSYSTEM_H
#define FDBSERVER_LOGSYSTEM_H
#pragma once

#include "TLogInterface.h"
#include "WorkerInterface.h"
#include "DatabaseConfiguration.h"
#include "flow/IndexedSet.h"
#include "fdbrpc/ReplicationPolicy.h"
#include "fdbrpc/Locality.h"
#include "fdbrpc/Replication.h"

struct DBCoreState;

class LogSet : NonCopyable, public ReferenceCounted<LogSet> {
public:
	std::vector<Reference<AsyncVar<OptionalInterface<TLogInterface>>>> logServers;
	std::vector<Reference<AsyncVar<OptionalInterface<TLogInterface>>>> logRouters;
	int32_t tLogWriteAntiQuorum;
	int32_t tLogReplicationFactor;
	std::vector< LocalityData > tLogLocalities; // Stores the localities of the log servers
	IRepPolicyRef tLogPolicy;
	LocalitySetRef logServerSet;
	std::vector<int> logIndexArray;
	std::map<int,LocalityEntry>	logEntryMap;
	bool isLocal;
	int32_t hasBestPolicy;
	int8_t locality;

	LogSet() : tLogWriteAntiQuorum(0), tLogReplicationFactor(0), isLocal(true), hasBestPolicy(HasBestPolicyId), locality(-99) {}

	int bestLocationFor( Tag tag ) {
		if(hasBestPolicy == HasBestPolicyNone) {
			return -1;
		} else if(hasBestPolicy == HasBestPolicyId) {
			//This policy supports upgrades from 5.X
			if(tag == txsTag) return txsTagOld % logServers.size();
			return tag.id % logServers.size();
		} else {
			//Unsupported policy
			ASSERT(false);
			throw internal_error();
		}
	}

	void updateLocalitySet() {
		LocalityMap<int>* logServerMap;
		logServerSet = LocalitySetRef(new LocalityMap<int>());
		logServerMap = (LocalityMap<int>*) logServerSet.getPtr();

		logEntryMap.clear();
		logIndexArray.clear();
		logIndexArray.reserve(logServers.size());

		for( int i = 0; i < logServers.size(); i++ ) {
			if (logServers[i]->get().present()) {
				logIndexArray.push_back(i);
				ASSERT(logEntryMap.find(i) == logEntryMap.end());
				logEntryMap[logIndexArray.back()] = logServerMap->add(logServers[i]->get().interf().locality, &logIndexArray.back());
			}
		}
	}

	void updateLocalitySet( vector<WorkerInterface> const& workers ) {
		LocalityMap<int>* logServerMap;

		logServerSet = LocalitySetRef(new LocalityMap<int>());
		logServerMap = (LocalityMap<int>*) logServerSet.getPtr();

		logEntryMap.clear();
		logIndexArray.clear();
		logIndexArray.reserve(workers.size());

		for( int i = 0; i < workers.size(); i++ ) {
			ASSERT(logEntryMap.find(i) == logEntryMap.end());
			logIndexArray.push_back(i);
			logEntryMap[logIndexArray.back()] = logServerMap->add(workers[i].locality, &logIndexArray.back());
		}
	}

	void getPushLocations( std::vector<Tag> const& tags, std::vector<int>& locations, int locationOffset ) {
		newLocations.clear();
		alsoServers.clear();
		resultEntries.clear();

		if(hasBestPolicy) {
			for(auto& t : tags) {
				if(t.locality == locality || t.locality == tagLocalitySpecial || locality == tagLocalitySpecial || (isLocal && t.locality == tagLocalityLogRouter)) {
					newLocations.push_back(bestLocationFor(t));
				}
			}
		}

		uniquify( newLocations );

		if (newLocations.size())
			alsoServers.reserve(newLocations.size());

		// Convert locations to the also servers
		for (auto location : newLocations) {
			ASSERT(logEntryMap[location]._id == location);
			locations.push_back(locationOffset + location);
			alsoServers.push_back(logEntryMap[location]);
		}

		// Run the policy, assert if unable to satify
		bool result = logServerSet->selectReplicas(tLogPolicy, alsoServers, resultEntries);
		ASSERT(result);

		// Add the new servers to the location array
		LocalityMap<int>* logServerMap = (LocalityMap<int>*) logServerSet.getPtr();
		for (auto entry : resultEntries) {
			locations.push_back(locationOffset + *logServerMap->getObject(entry));
		}
		//TraceEvent("getPushLocations").detail("Policy", tLogPolicy->info())
		//	.detail("Results", locations.size()).detail("Selection", logServerSet->size())
		//	.detail("Included", alsoServers.size()).detail("Duration", timer() - t);
	}

private:
	std::vector<LocalityEntry> alsoServers, resultEntries;
	std::vector<int> newLocations;
};

struct ILogSystem {
	// Represents a particular (possibly provisional) epoch of the log subsystem


	struct IPeekCursor {
		//clones the peek cursor, however you cannot call getMore() on the cloned cursor.
		virtual Reference<IPeekCursor> cloneNoMore() = 0;

		virtual void setProtocolVersion( uint64_t version ) = 0;

		//if hasMessage() returns true, getMessage() or reader() can be called.
		//does not modify the cursor
		virtual bool hasMessage() = 0;

		//pre: only callable if hasMessage() returns true
		//return the tags associated with the message for teh current sequence
		virtual std::vector<Tag> getTags() = 0;

		//pre: only callable if hasMessage() returns true
		//returns the arena containing the contents of getMessage() and reader()
		virtual Arena& arena() = 0;

		//pre: only callable if hasMessage() returns true
		//returns an arena reader for the next message
		//caller cannot call both getMessage() and reader()
		//the caller must advance the reader before calling nextMessage()
		virtual ArenaReader* reader() = 0;

		//pre: only callable if hasMessage() returns true
		//caller cannot call both getMessage() and reader()
		//return the contents of the message for the current sequence
		virtual StringRef getMessage() = 0;

		//pre: only callable after getMessage() or reader()
		//post: hasMessage() and version() have been updated
		//hasMessage() will never return false "in the middle" of a version (that is, if it does return false, version().subsequence will be zero)  < FIXME: Can we lose this property?
		virtual void nextMessage() = 0;

		//advances the cursor to the supplied LogMessageVersion, and updates hasMessage
		virtual void advanceTo(LogMessageVersion n) = 0;

		//returns immediately if hasMessage() returns true.
		//returns when either the result of hasMessage() or version() has changed.
		virtual Future<Void> getMore(int taskID = TaskTLogPeekReply) = 0;

		//returns when the failure monitor detects that the servers associated with the cursor are failed
		virtual Future<Void> onFailed() = 0;

		//returns false if:
		// (1) the failure monitor detects that the servers associated with the cursor is failed
		// (2) the interface is not present
		// (3) the cursor cannot return any more results
		virtual bool isActive() = 0;

		//returns true if the cursor cannot return any more results
		virtual bool isExhausted() = 0;

		// Returns the smallest possible message version which the current message (if any) or a subsequent message might have
		// (If hasMessage(), this is therefore the message version of the current message)
		virtual LogMessageVersion version() = 0;

		//So far, the cursor has returned all messages which both satisfy the criteria passed to peek() to create the cursor AND have (popped(),0) <= message version number <= version()
		//Other messages might have been skipped
		virtual Version popped() = 0;

		// Returns the maximum version known to have been pushed (not necessarily durably) into the log system (0 is always a possible result!)
		virtual Version getMaxKnownVersion() { return 0; }

		virtual void addref() = 0;

		virtual void delref() = 0;
	};

	struct ServerPeekCursor : IPeekCursor, ReferenceCounted<ServerPeekCursor> {
		Reference<AsyncVar<OptionalInterface<TLogInterface>>> interf;
		Tag tag;

		TLogPeekReply results;
		ArenaReader rd;
		LogMessageVersion messageVersion, end;
		Version poppedVersion;
		int32_t messageLength;
		std::vector<Tag> tags;
		bool hasMsg;
		Future<Void> more;
		UID randomID;
		bool returnIfBlocked;

		bool parallelGetMore;
		int sequence;
		Deque<Future<TLogPeekReply>> futureResults;
		Future<Void> interfaceChanged;

		ServerPeekCursor( Reference<AsyncVar<OptionalInterface<TLogInterface>>> const& interf, Tag tag, Version begin, Version end, bool returnIfBlocked, bool parallelGetMore );

		ServerPeekCursor( TLogPeekReply const& results, LogMessageVersion const& messageVersion, LogMessageVersion const& end, int32_t messageLength, bool hasMsg, Version poppedVersion, Tag tag );

		virtual Reference<IPeekCursor> cloneNoMore();

		virtual void setProtocolVersion( uint64_t version );

		virtual Arena& arena();

		virtual ArenaReader* reader();

		virtual bool hasMessage();

		virtual void nextMessage();

		virtual StringRef getMessage();

		virtual std::vector<Tag> getTags();

		virtual void advanceTo(LogMessageVersion n);

		virtual Future<Void> getMore(int taskID = TaskTLogPeekReply);

		virtual Future<Void> onFailed();

		virtual bool isActive();

		virtual bool isExhausted();

		virtual LogMessageVersion version();

		virtual Version popped();

		virtual void addref() {
			ReferenceCounted<ServerPeekCursor>::addref();
		}

		virtual void delref() {
			ReferenceCounted<ServerPeekCursor>::delref();
		}

		virtual Version getMaxKnownVersion() { return results.maxKnownVersion; }
	};

	struct MergedPeekCursor : IPeekCursor, ReferenceCounted<MergedPeekCursor> {
		vector< Reference<IPeekCursor> > serverCursors;
		std::vector< std::pair<LogMessageVersion, int> > sortedVersions;
		Tag tag;
		int bestServer, currentCursor, readQuorum;
		Optional<LogMessageVersion> nextVersion;
		LogMessageVersion messageVersion;
		bool hasNextMessage;
		UID randomID;
		int tLogReplicationFactor;
		IRepPolicyRef tLogPolicy;

		MergedPeekCursor( std::vector<Reference<AsyncVar<OptionalInterface<TLogInterface>>>> const& logServers, int bestServer, int readQuorum, Tag tag, Version begin, Version end, bool parallelGetMore );

		MergedPeekCursor( vector< Reference<IPeekCursor> > const& serverCursors, LogMessageVersion const& messageVersion, int bestServer, int readQuorum, Optional<LogMessageVersion> nextVersion );

		// if server_cursors[c]->hasMessage(), then nextSequence <= server_cursors[c]->sequence() and there are no messages known to that server with sequences in [nextSequence,server_cursors[c]->sequence())

		virtual Reference<IPeekCursor> cloneNoMore();

		virtual void setProtocolVersion( uint64_t version );

		virtual Arena& arena();

		virtual ArenaReader* reader();

		void calcHasMessage();

		void updateMessage();

		virtual bool hasMessage();

		virtual void nextMessage();

		virtual StringRef getMessage();

		virtual std::vector<Tag> getTags();

		virtual void advanceTo(LogMessageVersion n);

		virtual Future<Void> getMore(int taskID = TaskTLogPeekReply);

		virtual Future<Void> onFailed();

		virtual bool isActive();

		virtual bool isExhausted();

		virtual LogMessageVersion version();

		virtual Version popped();

		virtual void addref() {
			ReferenceCounted<MergedPeekCursor>::addref();
		}

		virtual void delref() {
			ReferenceCounted<MergedPeekCursor>::delref();
		}
	};

	struct SetPeekCursor : IPeekCursor, ReferenceCounted<SetPeekCursor> {
		std::vector<Reference<LogSet>> logSets;
		std::vector< std::vector< Reference<IPeekCursor> > > serverCursors;
		Tag tag;
		int bestSet, bestServer, currentSet, currentCursor;
		LocalityGroup localityGroup;
		std::vector< std::pair<LogMessageVersion, int> > sortedVersions;
		Optional<LogMessageVersion> nextVersion;
		LogMessageVersion messageVersion;
		bool hasNextMessage;
		bool useBestSet;
		UID randomID;

		SetPeekCursor( std::vector<Reference<LogSet>> const& logSets, int bestSet, int bestServer, Tag tag, Version begin, Version end, bool parallelGetMore );

		virtual Reference<IPeekCursor> cloneNoMore();

		virtual void setProtocolVersion( uint64_t version );

		virtual Arena& arena();

		virtual ArenaReader* reader();

		void calcHasMessage();

		void updateMessage(int logIdx, bool usePolicy);

		virtual bool hasMessage();

		virtual void nextMessage();

		virtual StringRef getMessage();

		virtual std::vector<Tag> getTags();

		virtual void advanceTo(LogMessageVersion n);

		virtual Future<Void> getMore(int taskID = TaskTLogPeekReply);

		virtual Future<Void> onFailed();

		virtual bool isActive();

		virtual bool isExhausted();

		virtual LogMessageVersion version();

		virtual Version popped();

		virtual void addref() {
			ReferenceCounted<SetPeekCursor>::addref();
		}

		virtual void delref() {
			ReferenceCounted<SetPeekCursor>::delref();
		}
	};

	struct MultiCursor : IPeekCursor, ReferenceCounted<MultiCursor> {
		std::vector<Reference<IPeekCursor>> cursors;
		std::vector<LogMessageVersion> epochEnds;
		Version poppedVersion;

		MultiCursor( std::vector<Reference<IPeekCursor>> cursors, std::vector<LogMessageVersion> epochEnds );

		virtual Reference<IPeekCursor> cloneNoMore();

		virtual void setProtocolVersion( uint64_t version );

		virtual Arena& arena();

		virtual ArenaReader* reader();

		virtual bool hasMessage();

		virtual void nextMessage();

		virtual StringRef getMessage();

		virtual std::vector<Tag> getTags();

		virtual void advanceTo(LogMessageVersion n);

		virtual Future<Void> getMore(int taskID = TaskTLogPeekReply);

		virtual Future<Void> onFailed();

		virtual bool isActive();

		virtual bool isExhausted();

		virtual LogMessageVersion version();

		virtual Version popped();

		virtual void addref() {
			ReferenceCounted<MultiCursor>::addref();
		}

		virtual void delref() {
			ReferenceCounted<MultiCursor>::delref();
		}
	};

	virtual void addref() = 0;
	virtual void delref() = 0;

	virtual std::string describe() = 0;
	virtual UID getDebugID() = 0;

	virtual void toCoreState( DBCoreState& ) = 0;

	virtual Future<Void> onCoreStateChanged() = 0;
		// Returns if and when the output of toCoreState() would change (for example, when older logs can be discarded from the state)

	virtual void coreStateWritten( DBCoreState const& newState ) = 0;
	    // Called when a core state has been written to the coordinators

	virtual Future<Void> onError() = 0;
		// Never returns normally, but throws an error if the subsystem stops working

	//Future<Void> push( UID bundle, int64_t seq, VectorRef<TaggedMessageRef> messages );
	virtual Future<Void> push( Version prevVersion, Version version, Version knownCommittedVersion, struct LogPushData& data, Optional<UID> debugID = Optional<UID>() ) = 0;
		// Waits for the version number of the bundle (in this epoch) to be prevVersion (i.e. for all pushes ordered earlier)
		// Puts the given messages into the bundle, each with the given tags, and with message versions (version, 0) - (version, N)
		// Changes the version number of the bundle to be version (unblocking the next push)
		// Returns when the preceding changes are durable.  (Later we will need multiple return signals for diffferent durability levels)
		// If the current epoch has ended, push will not return, and the pushed messages will not be visible in any subsequent epoch (but may become visible in this epoch)

	//Future<PeekResults> peek( int64_t begin_epoch, int64_t begin_seq, int tag );
	virtual Reference<IPeekCursor> peek( Version begin, Tag tag, bool parallelGetMore = false ) = 0;
		// Returns (via cursor interface) a stream of messages with the given tag and message versions >= (begin, 0), ordered by message version
		// If pop was previously or concurrently called with upTo > begin, the cursor may not return all such messages.  In that case cursor->popped() will
		// be greater than begin to reflect that.

	virtual Reference<IPeekCursor> peekSingle( Version begin, Tag tag, vector<pair<Version,Tag>> history = vector<pair<Version,Tag>>() ) = 0;
		// Same contract as peek(), but blocks until the preferred log server(s) for the given tag are available (and is correspondingly less expensive)

	virtual void pop( Version upTo, Tag tag ) = 0;
		// Permits, but does not require, the log subsystem to strip `tag` from any or all messages with message versions < (upTo,0)
		// The popping of any given message may be arbitrarily delayed.

	virtual Future<Void> confirmEpochLive( Optional<UID> debugID = Optional<UID>() ) = 0;
		// Returns success after confirming that pushes in the current epoch are still possible

	virtual Future<Void> endEpoch() = 0;
		// Ends the current epoch without starting a new one

	static Reference<ILogSystem> fromServerDBInfo( UID const& dbgid, struct ServerDBInfo const& db );
	static Reference<ILogSystem> fromLogSystemConfig( UID const& dbgid, struct LocalityData const&, struct LogSystemConfig const&, bool excludeRemote = false );
		// Constructs a new ILogSystem implementation from the given ServerDBInfo/LogSystemConfig.  Might return a null reference if there isn't a fully recovered log system available.
		// The caller can peek() the returned log system and can push() if it has version numbers reserved for it and prevVersions

	static Reference<ILogSystem> fromOldLogSystemConfig( UID const& dbgid, struct LocalityData const&, struct LogSystemConfig const& );
		// Constructs a new ILogSystem implementation from the old log data within a ServerDBInfo/LogSystemConfig.  Might return a null reference if there isn't a fully recovered log system available.

	static Future<Void> recoverAndEndEpoch(Reference<AsyncVar<Reference<ILogSystem>>> const& outLogSystem, UID const& dbgid, DBCoreState const& oldState, FutureStream<TLogRejoinRequest> const& rejoins, LocalityData const& locality);
		// Constructs a new ILogSystem implementation based on the given oldState and rejoining log servers
		// Ensures that any calls to push or confirmEpochLive in the current epoch but strictly later than change_epoch will not return
		// Whenever changes in the set of available log servers require restarting recovery with a different end sequence, outLogSystem will be changed to a new ILogSystem

	virtual Version getEnd() = 0;
		// Call only on an ILogSystem obtained from recoverAndEndEpoch()
		// Returns the first unreadable version number of the recovered epoch (i.e. message version numbers < (get_end(), 0) will be readable)

	virtual Future<Reference<ILogSystem>> newEpoch( struct RecruitFromConfigurationReply const& recr, Future<struct RecruitRemoteFromConfigurationReply> const& fRemoteWorkers, DatabaseConfiguration const& config, LogEpoch recoveryCount, int8_t primaryLocality, int8_t remoteLocality ) = 0;
		// Call only on an ILogSystem obtained from recoverAndEndEpoch()
		// Returns an ILogSystem representing a new epoch immediately following this one.  The new epoch is only provisional until the caller updates the coordinated DBCoreState

	virtual LogSystemConfig getLogSystemConfig() = 0;
		// Returns the physical configuration of this LogSystem, that could be used to construct an equivalent LogSystem using fromLogSystemConfig()

	virtual Standalone<StringRef> getLogsValue() = 0;

	virtual Future<Void> onLogSystemConfigChange() = 0;
		// Returns when the log system configuration has changed due to a tlog rejoin.

	virtual void getPushLocations( std::vector<Tag> const& tags, vector<int>& locations ) = 0;

	virtual bool hasRemoteLogs() = 0;

	virtual void addRemoteTags( int logSet, std::vector<Tag> const& originalTags, std::vector<int>& tags ) = 0;

	virtual Tag getRandomRouterTag() = 0;

	virtual void stopRejoins() = 0;
};

struct LengthPrefixedStringRef {
	// Represents a pointer to a string which is prefixed by a 4-byte length
	// A LengthPrefixedStringRef is only pointer-sized (8 bytes vs 12 bytes for StringRef), but the corresponding string is 4 bytes bigger, and
	// substring operations aren't efficient as they are with StringRef.  It's a good choice when there might be lots of references to the same
	// exact string.

	uint32_t* length;

	StringRef toStringRef() const { ASSERT(length); return StringRef( (uint8_t*)(length+1), *length ); }
	int expectedSize() const { ASSERT(length); return *length; }
	uint32_t* getLengthPtr() const { return length; }

	LengthPrefixedStringRef() : length(NULL) {}
	LengthPrefixedStringRef(uint32_t* length) : length(length) {}
};

template<class T>
struct CompareFirst {
	bool operator() (T const& lhs, T const& rhs) const {
		return lhs.first < rhs.first;
	}
};

struct LogPushData : NonCopyable {
	// Log subsequences have to start at 1 (the MergedPeekCursor relies on this to make sure we never have !hasMessage() in the middle of data for a version

	explicit LogPushData(Reference<ILogSystem> logSystem) : logSystem(logSystem), subsequence(1) {
		int totalSize = 0;
		for(auto& log : logSystem->getLogSystemConfig().tLogs) {
			if(log.isLocal) {
				totalSize += log.tLogs.size();
			}
		}
		tags.resize( totalSize );
		for(int i = 0; i < tags.size(); i++) {
			messagesWriter.push_back( BinaryWriter( AssumeVersion(currentProtocolVersion) ) );
		}
	}

	// addTag() adds a tag for the *next* message to be added
	void addTag( Tag tag ) {
		next_message_tags.push_back( tag );
	}

	void addMessage( StringRef rawMessageWithoutLength, bool usePreviousLocations = false ) {
		if( !usePreviousLocations ) {
			prev_tags.clear();
			if(logSystem->hasRemoteLogs()) {
				prev_tags.push_back( logSystem->getRandomRouterTag() );
			}
			for(auto& tag : next_message_tags) {
				prev_tags.push_back(tag);
			}
			msg_locations.clear();
			logSystem->getPushLocations( prev_tags, msg_locations );
			next_message_tags.clear();
		}
		uint32_t subseq = this->subsequence++;
		for(int loc : msg_locations) {
			for(auto& tag : prev_tags)
				addTagToLoc( tag, loc );

			messagesWriter[loc] << uint32_t(rawMessageWithoutLength.size() + sizeof(subseq) + sizeof(uint16_t) + sizeof(Tag)*prev_tags.size()) << subseq << uint16_t(prev_tags.size());
			for(auto& tag : prev_tags)
				messagesWriter[loc] << tag;
			messagesWriter[loc].serializeBytes(rawMessageWithoutLength);
		}
	}

	template <class T>
	void addTypedMessage( T const& item ) {
		prev_tags.clear();
		if(logSystem->hasRemoteLogs()) {
			prev_tags.push_back( logSystem->getRandomRouterTag() );
		}
		for(auto& tag : next_message_tags) {
			prev_tags.push_back(tag);
		}
		msg_locations.clear();
		logSystem->getPushLocations( prev_tags, msg_locations );
		
		uint32_t subseq = this->subsequence++;
		for(int loc : msg_locations) {
			for(auto& tag : prev_tags)
				addTagToLoc( tag, loc );

			// FIXME: memcpy after the first time
			BinaryWriter& wr = messagesWriter[loc];
			int offset = wr.getLength();
			wr << uint32_t(0) << subseq << uint16_t(prev_tags.size());
			for(auto& tag : prev_tags)
				wr << tag;
			wr << item;
			*(uint32_t*)((uint8_t*)wr.getData() + offset) = wr.getLength() - offset - sizeof(uint32_t);
		}
		next_message_tags.clear();
	}

	Arena getArena() { return arena; }
	StringRef getMessages(int loc) {
		return StringRef( arena, messagesWriter[loc].toStringRef() );  // FIXME: Unnecessary copy!
	}
	VectorRef<TagMessagesRef> getTags(int loc) {
		VectorRef<TagMessagesRef> r;
		for(auto& t : tags[loc])
			r.push_back( arena, t.value );
		return r;
	}

private:
	void addTagToLoc( Tag tag, int loc ) {
		auto it = tags[loc].find(tag);
		if (it == tags[loc].end()) {
			it = tags[loc].insert(mapPair( tag, TagMessagesRef() ));
			it->value.tag = it->key;
		}
		it->value.messageOffsets.push_back( arena, messagesWriter[loc].getLength() );
	}

	Reference<ILogSystem> logSystem;
	Arena arena;
	vector<Tag> next_message_tags;
	vector<Tag> prev_tags;
	vector<Map<Tag, TagMessagesRef>> tags;
	vector<BinaryWriter> messagesWriter;
	vector<int> msg_locations;
	uint32_t subsequence;
};

#endif
