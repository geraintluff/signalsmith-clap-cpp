#pragma once

#include "clap/events.h"

#include <vector>
#include <array>
#include <optional>
#include <cmath>

namespace signalsmith { namespace clap {

/* This helper handles CLAP note events, and returns "note tasks", which are sub-blocks for processing each note.  A note's tasks will have a consistent `voiceIndex` (up to the specified polyphony), exclusive to that note it's `.stop()`ed or stolen.

When you hand it an event (and it returns `true`), it returns tasks to process any affected notes up to that point.  You can also request all notes be processed up to a certain block index, which should be used for completing a block, or for any sample-accurate parameter/etc. changes which affect all notes.

It implements voice-stealing based on time since a note's release (if released) or attack.  This is represented by a note-task with `stateKill`.  The length (`processFrom`/`processTo`) of this task will not overlap with the new note - which unavoidably means it *may* be 0, in which case you can process a bit more to avoid clicks at your discretion.
*/
struct NoteManager {
	enum State{stateDown, stateLegato, stateContinue, stateUp, stateRelease, stateKill};
	
	// 2 for default MIDI, 48 for most MPE
	double pitchWheelRange = 2;
	struct NoteMod;
	
	struct Note {
		// Note info
		size_t voiceIndex;
		double key, velocity;
		// Note expression (possibly translated from MIDI CCs)
		double volume = 1, pan = 0.5, mod = 0, expression = 1, brightness = 0.5, pressure = 1;
		int16_t port, channel;
		// Processing task info
		State state;
		uint32_t processFrom, processTo;
		
		bool released() const {
			return state == stateUp || state == stateRelease || state == stateKill;
		}
		
		// `other` may contain wildcards (-1), but `this` must not
		bool match(const Note &other) const {
			if (other.noteId != -1) return noteId == other.noteId;
			if (released()) return false;
			if (other.port != -1 && other.port != port) return false;
			if (other.channel != -1 && other.channel != channel) return false;
			if (other.baseKey != -1 && other.baseKey != baseKey) return false;
			return true;
		}
		bool match(const NoteMod &noteMod) const {
			if (noteMod.noteId != -1) return noteId == noteMod.noteId;
			if (released()) return false;
			if (noteMod.port != -1 && noteMod.port != port) return false;
			if (noteMod.channel != -1 && noteMod.channel != channel) return false;
			if (noteMod.baseKey != -1 && noteMod.baseKey != baseKey) return false;
			return true;
		}
		// A few different event types have the same fields, this catches all of them
		template<class ClapEvent>
		bool matchEvent(const ClapEvent &clapEvent, bool includeReleased=false) const {
			if (clapEvent.note_id != -1) return clapEvent.note_id == noteId;
			if (released() && !includeReleased) return false;
			if (clapEvent.port_index >= 0 && clapEvent.port_index != port) return false;
			if (clapEvent.channel >= 0 && clapEvent.channel != channel) return false;
			if (clapEvent.key >= 0 && clapEvent.key != baseKey) return false;
			return true;
		}
		
		float killCost() const {
			return 1.0f/(age + 1) + 10 - (int)state;
		}
		
		size_t ageAt(uint32_t timeInBlock) const {
			return age + (timeInBlock - processFrom);
		}
		
		int32_t noteId;
		int16_t baseKey;
	private:
		friend class NoteManager;

		Note(size_t voiceIndex, const clap_event_note &e, State state=stateDown) : voiceIndex(voiceIndex), key(e.key), velocity(e.velocity), port(e.port_index), channel(e.channel), state(state), processFrom(e.header.time), processTo(e.header.time), noteId(e.note_id), baseKey(e.key) {}
		size_t age = 0; // since start/legato/up
	};
	struct NoteMod {
		uint32_t time;
		
		clap_note_expression expression;
		double value;

		int16_t port, channel;
		int32_t noteId;
		int16_t baseKey;

		void applyTo(Note &note) const {
			if (expression == CLAP_NOTE_EXPRESSION_TUNING) {
				note.key = note.baseKey + value;
			} else {
#ifdef LOG_EXPR
				LOG_EXPR(expression);
#endif
			}
		}
	};
	
	NoteManager(size_t polyphony=64, double pitchWheelRange=2) : pitchWheelRange(pitchWheelRange) {
		notes.reserve(polyphony);
		tasks.reserve(polyphony);
		reset();
	}
	
	size_t polyphony() const {
		return notes.capacity();
	}
	
	void reset() {
		notes.clear();
		tasks.clear();
		for (auto &channel : channelNoteExpressions) {
			channel = {
				1.0, // volume
				0.5, // pan
				0.0, // tuning
				0, // vibrato (modulation)
				1.0, // expression
				0.5, // brightness
				1.0, // pressure
			};
		}

		voiceIndexQueue.clear();
		for (size_t i = 0; i < polyphony(); ++i) {
			voiceIndexQueue.push_back(polyphony() - 1 - i);
		}
	}
	
	void startBlock() {
		tasks.clear();
		for (auto &n : notes) n.processFrom = n.processTo = 0;
	}
	const std::vector<Note> & processTo(uint32_t frames) {
		tasks.clear();
		for (auto &n : notes) {
			if (n.processFrom < frames) {
				n.processTo = frames;
				tasks.push_back(n);
				n.age += (n.processTo - n.processFrom);
				n.processFrom = frames;
				if (n.state == stateDown || n.state == stateLegato) {
					n.state = stateContinue;
				} else if (n.state == stateUp) {
					n.state = stateRelease;
				}
			}
		}
		return tasks;
	}

	// Gets a note ready, but don't do anything with it yet
	std::optional<Note> wouldStart(const clap_event_header *event) const {
		if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return {};
		event = translateEvent(event);
		if (event->type == CLAP_EVENT_NOTE_ON) {
			auto &noteEvent = *(const clap_event_note *)event;
			Note newNote{size_t(-1), noteEvent};
			if (newNote.noteId < 0) nextNoteId(newNote);
			applyChannelNoteExpressions(newNote);
			return {newNote};
		}
		return {};
	}

	// You should call this if you're not using a note-on, so the host gets a NOTE_END
	void ignore(const Note &newNote, const clap_output_events *eventsOut) {
		sendNoteEnd(newNote, eventsOut);
	}
	
	const std::vector<Note> & start(Note &newNote, const clap_output_events *eventsOut) {
		tasks.clear();
		if (notes.size() >= notes.capacity()) {
			// Kill an existing note
			size_t killIndex = 0;
			float killCost = 1e10;
			for (size_t i = 0; i < notes.size(); ++i) {
				auto cost = notes[i].killCost();
				if (cost < killCost) {
					killIndex = i;
					killCost = cost;
				}
			}
			auto &killNote = notes[killIndex];
			killNote.state = stateKill;
			killNote.processTo = newNote.processFrom;
			// Push this task even if it's zero length
			tasks.push_back(killNote);
			stop(killNote, eventsOut);
		}

		// We had at least one note left in capacity, so this is safe
		newNote.voiceIndex = voiceIndexQueue.back();
		voiceIndexQueue.pop_back();
		notes.push_back(newNote);
		return tasks;
	}
	
	const std::vector<Note> & legato(Note &newNote, const Note &existingNote, const clap_output_events *eventsOut) {
		tasks.clear();
		for (auto &n : notes) {
			if (n.match(existingNote)) {
				// Process the note
				addTask(n, newNote.processFrom);
				sendNoteEnd(n, eventsOut); // release the old note ID

				auto voiceIndex = existingNote.voiceIndex;
				n = newNote;
				n.voiceIndex = newNote.voiceIndex = voiceIndex;
				n.state = stateLegato;
				n.age = 0;
				break;
			}
		}
		return tasks;
	}

	std::optional<Note> wouldRelease(const clap_event_header *event) const {
		if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return {};
		event = translateEvent(event);
		if (event->type == CLAP_EVENT_NOTE_OFF || event->type == CLAP_EVENT_NOTE_CHOKE) {
			auto &noteEvent = *(const clap_event_note *)event;
			return {Note{size_t(-1), noteEvent, stateUp}}; // still includes any wildcards
		}
		return {};
	}

	const std::vector<Note> & release(Note &releaseNote) {
		// If this is a note-end event (or we don't care) then use the timestamp we already have
		return release(releaseNote, releaseNote.processFrom);
	}

	const std::vector<Note> & release(Note &releaseNote, uint32_t atBlockTime) {
		tasks.clear();
		for (auto &n : notes) {
			if (n.match(releaseNote)) {
				addTask(n, atBlockTime);
				n.state = stateUp;
				n.velocity = releaseNote.velocity;
				n.age = 0;
				releaseNote.voiceIndex = n.voiceIndex; // let the caller know which note we just released
				// Stop unless the note ID is a wildcard
				if (releaseNote.noteId != -1) break;
			}
		}
		return tasks;
	}

	std::optional<NoteMod> wouldModNotes(const clap_event_header *event) const {
		if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return {};
		event = translateEvent(event);
		if (event->type == CLAP_EVENT_NOTE_EXPRESSION) {
			auto &exprEvent = *(const clap_event_note_expression *)event;
			NoteMod noteMod{
				.time=exprEvent.header.time,
				.expression=exprEvent.expression_id,
				.value=exprEvent.value,
				.port=exprEvent.port_index,
				.channel=exprEvent.channel,
				.noteId=exprEvent.note_id,
				.baseKey=exprEvent.key
			};
			return {noteMod};
		}
		return {};
	}
	const std::vector<Note> & modNotes(const NoteMod &noteMod) {
		return modNotes(noteMod, noteMod.time);
	}
	const std::vector<Note> & modNotes(const NoteMod &noteMod, uint32_t atBlockTime) {
		tasks.clear();
		if (noteMod.noteId == -1 && noteMod.baseKey == -1 && noteMod.channel >= 0 && noteMod.channel < 16) {
			// We're generally not tracking CC state, but if we're translating MPE to note expressions then we store them for the case when notes start after the CCs
			channelNoteExpressions[noteMod.channel][noteMod.expression] = noteMod.value;
		}
		for (auto &n : notes) {
			if (n.match(noteMod)) {
				addTask(n, atBlockTime, true);
				noteMod.applyTo(n);
			}
		}
		return tasks;
	}

	// Start or stop notes as appropriate
	const std::vector<Note> & processEvent(const clap_event_header *event, const clap_output_events *eventsOut) {
		auto newNote = wouldStart(event);
		if (newNote) return start(*newNote, eventsOut);
		
		auto releaseNote = wouldRelease(event);
		if (releaseNote) return release(*releaseNote);
		
		auto modNote = wouldModNotes(event);
		if (modNote) return modNotes(*modNote);

		tasks.clear();
		return tasks;
	}
	
	// This note has finished - we no longer want any other tasks about it, and its voice can be reassigned
	void stop(const Note &noteToStop, const clap_output_events *eventsOut) {
		for (auto &n : notes) {
			if (n.match(noteToStop)) {
				sendNoteEnd(n, eventsOut);
				voiceIndexQueue.push_back(n.voiceIndex);

				if (notes.size() <= 1) { // final note
					notes.clear();
				} else {
					if (&n != &notes.back()) {
						// Move the last note into this slot
						n = notes.back();
					}
					notes.pop_back();
				}
				return;
			}
		}
	}

	const std::vector<Note> & activeNotes() const {
		return notes;
	}
	
	auto begin() const {
		return notes.begin();
	}
	auto end() const {
		return notes.end();
	}
	
private:
	mutable uint32_t internalNoteId = 2;
	void nextNoteId(Note &newNote) const {
		newNote.noteId = -int32_t(internalNoteId);
		if (++internalNoteId >= 0x7FFFFFFF) internalNoteId = 2;
	}

	std::vector<Note> notes;
	std::vector<Note> tasks;
	std::vector<size_t> voiceIndexQueue;
	
	void addTask(Note &n, uint32_t processTo, bool noStateChange=false) {
		// Skip zero-length tasks for non-event states, or if we know that the event state isn't about to be overwritten
		if (n.processFrom >= processTo && (noStateChange || n.state == stateContinue || n.state == stateRelease)) return;
		n.processTo = processTo;
		tasks.push_back(n);
		n.age += (processTo - n.processFrom);
		n.processFrom = processTo;
	}
	
	void sendNoteEnd(const Note &n, const clap_output_events *eventsOut) {
		if (n.noteId >= 0) {
			// Let the host know the note isn't available for modulation any more
			clap_event_note stopEvent{
				.header={
					.size=sizeof(clap_event_note),
					.time=n.processFrom,
					.space_id=CLAP_CORE_EVENT_SPACE_ID,
					.type=CLAP_EVENT_NOTE_END,
					.flags=CLAP_EVENT_DONT_RECORD
				},
				.note_id=n.noteId,
				.port_index=n.port,
				.channel=n.port,
				.key=n.baseKey,
				.velocity=0
			};
			eventsOut->try_push(eventsOut, &stopEvent.header);
		}
	}
	
	union {
		mutable clap_event_note noteEvent;
		mutable clap_event_note_expression noteExprEvent;
	};

	const clap_event_header * translateEvent(const clap_event_header *event) const {
		if (event->type != CLAP_EVENT_MIDI) return event;
		const clap_event_midi &midiEvent = *(const clap_event_midi *)event;

		unsigned char eventType = midiEvent.data[0]&0xF0;
		unsigned char channel = midiEvent.data[0]&0x0F;
		if (eventType == 0x80 || eventType == 0x90) {
			noteEvent = {
				.header=*event,
				.note_id=-1,
				.port_index=int16_t(midiEvent.port_index),
				.channel=channel,
				.key=int16_t(midiEvent.data[1]),
				.velocity=midiEvent.data[2]/127.0
			};
			noteEvent.header.size = sizeof(noteEvent);
			noteEvent.header.type = (eventType == 0x90 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
			return &noteEvent.header;
		} else if (eventType == 0xC0) {
			return event; // we don't handle Program Change
		} else {
			noteExprEvent = {
				.header=*event,
				.expression_id=-1,
				.note_id=-1,
				.port_index=int16_t(midiEvent.port_index),
				.channel=channel,
				.key=-1,
				.value=0,
			};
			noteExprEvent.header.size = sizeof(noteExprEvent);
			noteExprEvent.header.type = CLAP_EVENT_NOTE_EXPRESSION;

			if (eventType == 0xA0) { // Polyphonic aftertouch -> note pressure
				noteExprEvent.key = midiEvent.data[1];
				noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
				noteExprEvent.value = midiEvent.data[2]/127.0;
			} else if (eventType == 0xB0) { // MIDI CC
				auto cc = midiEvent.data[1];
				noteExprEvent.value = midiEvent.data[2]/127.0;
				if (cc == 1) noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_VIBRATO;
				if (cc == 4) noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_BRIGHTNESS; // foot pedal, why not
				if (cc == 10) noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_PAN;
				if (cc == 11) noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_EXPRESSION;
				if (cc == 7) {
					noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_VOLUME;
					noteExprEvent.value = std::pow(midiEvent.data[2]/100.0, 5.8); // volume 0-4, with CC=100 -> volume=1
				}
			} else if (eventType == 0xD0) { // Channel aftertouch -> note pressure
				noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
				noteExprEvent.value = midiEvent.data[1]/127.0;
			} else if (eventType == 0xE0) { // Pitch wheel
				noteExprEvent.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
				noteExprEvent.value = (midiEvent.data[1] + midiEvent.data[2]*128 - 0x2000)*pitchWheelRange/0x2000;
			}
			if (noteExprEvent.expression_id == -1) return event; // no translation
			return &noteExprEvent.header;
		}
	}

	// Default note expressions taken from MPE-translated CCs
	std::array<std::array<double, 7>, 16> channelNoteExpressions;
	void applyChannelNoteExpressions(Note &note) const {
		if (note.channel < 0 || note.channel >= 16) return;
		note.volume = channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_VOLUME];
		note.pan = channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_PAN];
		note.key += channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_TUNING];
		note.mod = channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_VIBRATO];
		note.expression = channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_EXPRESSION];
		note.brightness = channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_BRIGHTNESS];
		note.pressure = channelNoteExpressions[note.channel][CLAP_NOTE_EXPRESSION_PRESSURE];
	}
};

}} // namespace
