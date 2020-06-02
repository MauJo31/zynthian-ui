/**	Sequence class methods implementation **/

#include "sequence.h"

Sequence::Sequence()
{
}

Sequence::~Sequence()
{
}

void Sequence::addPattern(uint32_t position, Pattern* pattern)
{
	// Find and remove overlapping patterns
	uint32_t nStart = position;
	uint32_t nEnd = nStart + pattern->getLength();
	for(uint32_t nClock = 0; nClock <= position + pattern->getLength(); ++nClock)
	{
		if(m_mPatterns.find(nClock) != m_mPatterns.end())
		{
			Pattern* pPattern = m_mPatterns[nClock];
			uint32_t nExistingStart = nClock;
			uint32_t nExistingEnd = nExistingStart + pPattern->getLength();

			if((nStart >= nExistingStart && nStart < nExistingEnd) || (nEnd > nExistingStart && nEnd <= nExistingEnd))
			{
				// Found overlapping pattern so remove from sequence but don't delete (that is responsibility of PatternManager)
				m_mPatterns.erase(nClock);
				if(m_nCurrentPattern == nClock)
					m_nCurrentPattern = -1;
			}
		}
	}
	m_mPatterns[position] = pattern;
	if(m_nSequenceLength < position + pattern->getLength())
		m_nSequenceLength = position + pattern->getLength();
}

void Sequence::removePattern(uint32_t position)
{
	m_mPatterns.erase(position);
	if(m_nCurrentPattern == position)
		m_nCurrentPattern = -1;
	updateLength();
}

Pattern* Sequence::getPattern(uint32_t position)
{
	auto it = m_mPatterns.find(position);
	if(it == m_mPatterns.end())
		return NULL;
	return it->second;
}

uint8_t Sequence::getChannel()
{
	return m_nChannel;
}

void Sequence::setChannel(uint8_t channel)
{
	if(channel > 15)
		return;
	m_nChannel = channel;
}

uint8_t Sequence::getOutput()
{
	return m_nOutput;
}

void Sequence::setOutput(uint8_t output)
{
	m_nOutput = output;
}

uint8_t Sequence::getPlayMode()
{
	return m_nMode;
}

void Sequence::setPlayMode(uint8_t mode)
{
	if(mode > LASTPLAYMODE)
		return;
	m_nMode = mode;
}

uint8_t Sequence::getPlayState()
{
	return m_nState;
}

void Sequence::setPlayState(uint8_t state)
{
	if(state > LASTPLAYSTATUS)
		return;
	if(state == STOPPING)
		switch(m_nMode)
		{
			case DISABLED:
			case ONESHOT:
			case LOOP:
				m_nState = STOPPED;
				return;
		}
	else if(state == STARTING)
		m_nDivCount = 0; //!@todo What should div count be?
	if(m_nSequenceLength)
		m_nState = state;
	else
		m_nState = STOPPED;
}

void Sequence::togglePlayState()
{
	if(m_nState == STOPPED || m_nState == STOPPING)
		setPlayState(STARTING);
	else
		setPlayState(STOPPING);
}

bool Sequence::clock(uint32_t nTime, bool bSync)
{
	// Clock cycle - update position and associated counters, status, etc.
	if(bSync && m_nState == STARTING)
	{
		m_nState = PLAYING;
	}
	if(m_nState == STOPPED || m_nState == STARTING || m_nDivCount-- != 0)
		return false;
	//printf("Sequence::clock PlayState: %d pos: %d div: %d time: %d PatternCursor: %d\n", m_nState, m_nPosition, m_nClkPerStep, nTime, m_nPatternCursor);
	m_nCurrentTime = nTime;
	m_nDivCount = m_nClkPerStep - 1;
	if(m_nPosition >= m_nSequenceLength)
	{
		// Reached end of sequence
//		m_nPosition = 0;
		if(m_nState == STOPPING)
		{
			m_nState = STOPPED;
			return false;
		}
		switch(m_nMode)
		{
			case ONESHOT:
			case DISABLED:
			case ONESHOTALL:
				m_nState = STOPPED;
				return false;
			case LOOP:
			case LOOPALL:
				m_nPosition = 0;
				break;
		}
	}
	if(m_mPatterns.find(m_nPosition) != m_mPatterns.end())
	{
		// Play head at start of pattern
		m_nCurrentPattern = m_nPosition;
		m_nPatternCursor = 0;
		m_nNextEvent = 0;
		m_nClkPerStep = m_mPatterns[m_nCurrentPattern]->getClocksPerStep();
		if(m_nClkPerStep == 0)
			m_nClkPerStep = 1;
		m_nEventValue = -1;
		//printf("At start of pattern. Pos: %d clkPerStep: %d\n", m_nPosition, m_nClkPerStep);
	}
	else if(m_nCurrentPattern >= 0 && m_nPatternCursor >= m_mPatterns[m_nCurrentPattern]->getSteps())
	{
		// Beyond pattern but not at start of another (between patterns)
		m_nCurrentPattern = -1;
		m_nNextEvent = -1;
		m_nPatternCursor = 0;
		m_nClkPerStep = 1;
		m_nEventValue = -1;
	}
	else
	{
		// Within a pattern
		++m_nPatternCursor;
	}
	m_nPosition += m_nClkPerStep;
	return true;
}

SEQ_EVENT* Sequence::getEvent()
{
	//printf("Sequence::getEvent state: %d pattern:%d nextevent:%d\n", m_nState, m_nCurrentPattern, m_nNextEvent);
	// This function is called repeatedly for each clock period until no more events are available to populate JACK MIDI output schedule
	static SEQ_EVENT seqEvent; // A MIDI event timestamped for some imminent or future time
	if(m_nState == STOPPED || m_nCurrentPattern < 0 || m_nNextEvent < 0)
		return NULL;
	// Sequence is being played and playhead is within a pattern
	Pattern* pPattern = m_mPatterns[m_nCurrentPattern];
	StepEvent* pEvent = pPattern->getEventAt(m_nNextEvent); // Don't advance event here because need to interpolate
	if(pEvent && pEvent->getPosition() == m_nPatternCursor)
	{
		if(m_nEventValue == pEvent->getValue2end())
		{
			// We have reached the end of interpolation so move on to next event
			m_nEventValue = -1;
			pEvent = pPattern->getEventAt(++m_nNextEvent);
			if(!pEvent || pEvent->getPosition() != m_nPatternCursor)
				return NULL;
		}
		if(m_nEventValue == -1)
		{
			// Have not yet started to interpolate value
			m_nEventValue = pEvent->getValue2start();
			seqEvent.time = m_nCurrentTime;
		}
		else if(pEvent->getValue2start() == m_nEventValue)
		{
			// Already processed start value
			m_nEventValue = pEvent->getValue2end(); //!@todo Currently just move straight to end value but should interpolate for CC
			seqEvent.time = m_nCurrentTime + pEvent->getDuration() * (pPattern->getClocksPerStep() - 1) * m_nSamplePerClock; // -1 to send note-off one clock before next step
		}
	}
	else
	{
		m_nEventValue = -1;
		return NULL;
	}
	seqEvent.msg.command = pEvent->getCommand() | m_nChannel;
	seqEvent.msg.value1 = pEvent->getValue1start();
	seqEvent.msg.value2 = m_nEventValue;
	//printf("sequence::getEvent Event %u,%u,%u at %u currentTime: %u duration: %u clkperstep: %u sampleperclock: %u\n", seqEvent.msg.command, seqEvent.msg.value1, seqEvent.msg.value2, seqEvent.time, m_nCurrentTime, pEvent->getDuration(), pPattern->getClocksPerStep(), m_nSamplePerClock);
	return &seqEvent;
}

void Sequence::updateLength()
{
	m_nSequenceLength = 0;
	for(auto it = m_mPatterns.begin(); it != m_mPatterns.end(); ++it)
		if(it->first + it->second->getLength() > m_nSequenceLength)
			m_nSequenceLength = it->first + it->second->getLength();
}

uint32_t Sequence::getLength()
{
	return m_nSequenceLength;
}

void Sequence::clear()
{
	m_mPatterns.clear();
	m_nSequenceLength = 0;
	m_nEventValue = -1;
	m_nCurrentPattern = -1;
	m_nNextEvent = -1;
	m_nPatternCursor = 0;
	m_nClkPerStep = 1;
	m_nDivCount = 0;
	m_nPosition = 0;
}

uint32_t Sequence::getStep()
{
	return m_nPatternCursor;
}

uint32_t Sequence::getPatternPlayhead()
{
	return m_nPatternCursor * m_nClkPerStep;
}

uint32_t Sequence::getPlayPosition()
{
	return m_nPosition;
}

void Sequence::setPlayPosition(uint32_t clock)
{
	if(m_mPatterns.size() < 1)
		return;

	// Find if we are within a pattern
	auto it = m_mPatterns.begin();
	for(; it != m_mPatterns.end(); ++it)
	{
		if(it->first > clock)
			break;
	}
	if(it != m_mPatterns.begin())
		--it;
	Pattern* pPattern = it->second;

	uint32_t nTime = clock - it->first; // Clock cycles since start of last (maybe current) pattern

	if(clock >= it->first + pPattern->getLength())
	{
		// Between patterns
		m_nCurrentPattern = -1;
		m_nPatternCursor = 0;
		m_nNextEvent = -1;
		m_nClkPerStep = 1;
		m_nEventValue = -1;
		m_nDivCount = 0;
	}
	else
	{
		// Within pattern
		m_nCurrentPattern = it->first;

		m_nClkPerStep = m_mPatterns[m_nCurrentPattern]->getClocksPerStep();
		if(m_nClkPerStep == 0)
			m_nClkPerStep = 1;

		m_nDivCount = (clock - m_nCurrentPattern) % m_nClkPerStep; // Clocks cycles until next step
		if(m_nDivCount)
			m_nDivCount = m_nClkPerStep - m_nDivCount - 1;

		m_nPosition = clock + m_nDivCount; // Position at next step

		m_nPatternCursor = (clock - m_nCurrentPattern) / m_nClkPerStep - 1;

		m_nNextEvent = -1;
		uint32_t nEvent = 0;
		while(StepEvent* pEvent = pPattern->getEventAt(nEvent++))
		{
			++m_nNextEvent;
			if(pEvent->getPosition() >= m_nPatternCursor)
				break;
		}
	}

	//printf("Sequence::setPlayPosition song pos: %d, pattern cursor: %d, next event: %d, divcount: %u\n", m_nPosition, m_nPatternCursor, m_nNextEvent, m_nDivCount);
}

uint32_t Sequence::getNextPattern(uint32_t previous)
{
	if(m_mPatterns.size() == 0)
		return 0xFFFFFFFF;
	if(previous == 0xFFFFFFFF)
		return m_mPatterns.begin()->first;
	auto it = m_mPatterns.find(previous);
	if(++it == m_mPatterns.end())
		return 0xFFFFFFFF;
	return it->first;
}

void Sequence::setGroup(uint8_t group)
{
    if(group <= 26)
        m_nGroup = group;
}

uint8_t Sequence::getGroup()
{
	return m_nGroup;
}

void Sequence::setTrigger(uint8_t trigger)
{
	if(trigger < 128)
		m_nTrigger = trigger;
	else
		m_nTrigger = 0xFF;
}

uint8_t Sequence::getTrigger()
{
	return m_nTrigger;
}
