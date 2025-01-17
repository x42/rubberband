/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Rubber Band Library
    An audio time-stretching and pitch-shifting library.
    Copyright 2007-2022 Particular Programs Ltd.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.

    Alternatively, if you have a valid commercial licence for the
    Rubber Band Library obtained by agreement with the copyright
    holders, you may redistribute and/or modify it under the terms
    described in that licence.

    If you wish to distribute code using the Rubber Band Library
    under terms other than those of the GNU General Public License,
    you must obtain a valid commercial licence before doing so.
*/

#include "R3Stretcher.h"

#include "../common/VectorOpsComplex.h"

#include <array>

namespace RubberBand {

R3Stretcher::R3Stretcher(Parameters parameters,
                         double initialTimeRatio,
                         double initialPitchScale,
                         Log log) :
    m_parameters(parameters),
    m_log(log),
    m_timeRatio(initialTimeRatio),
    m_pitchScale(initialPitchScale),
    m_formantScale(0.0),
    m_guide(Guide::Parameters(m_parameters.sampleRate), m_log),
    m_guideConfiguration(m_guide.getConfiguration()),
    m_channelAssembly(m_parameters.channels),
    m_inhop(1),
    m_prevInhop(1),
    m_prevOuthop(1),
    m_unityCount(0),
    m_startSkip(0),
    m_studyInputDuration(0),
    m_suppliedInputDuration(0),
    m_totalTargetDuration(0),
    m_consumedInputDuration(0),
    m_lastKeyFrameSurpassed(0),
    m_totalOutputDuration(0),
    m_mode(ProcessMode::JustCreated)
{
    m_log.log(1, "R3Stretcher::R3Stretcher: rate, options",
              m_parameters.sampleRate, m_parameters.options);
    m_log.log(1, "R3Stretcher::R3Stretcher: initial time ratio and pitch scale",
              m_timeRatio, m_pitchScale);

    double maxClassifierFrequency = 16000.0;
    if (maxClassifierFrequency > m_parameters.sampleRate/2) {
        maxClassifierFrequency = m_parameters.sampleRate/2;
    }
    int classificationBins =
        int(floor(m_guideConfiguration.classificationFftSize *
                  maxClassifierFrequency / m_parameters.sampleRate));
    
    BinSegmenter::Parameters segmenterParameters
        (m_guideConfiguration.classificationFftSize,
         classificationBins, m_parameters.sampleRate, 18);
    
    BinClassifier::Parameters classifierParameters
        (classificationBins, 9, 1, 10, 2.0, 2.0);

    int inRingBufferSize = m_guideConfiguration.longestFftSize * 2;
    int outRingBufferSize = m_guideConfiguration.longestFftSize * 16;

    for (int c = 0; c < m_parameters.channels; ++c) {
        m_channelData.push_back(std::make_shared<ChannelData>
                                (segmenterParameters,
                                 classifierParameters,
                                 m_guideConfiguration.longestFftSize,
                                 inRingBufferSize,
                                 outRingBufferSize));
        for (auto band: m_guideConfiguration.fftBandLimits) {
            int fftSize = band.fftSize;
            m_channelData[c]->scales[fftSize] =
                std::make_shared<ChannelScaleData>
                (fftSize, m_guideConfiguration.longestFftSize);
        }
    }
    
    for (auto band: m_guideConfiguration.fftBandLimits) {
        int fftSize = band.fftSize;
        GuidedPhaseAdvance::Parameters guidedParameters
            (fftSize, m_parameters.sampleRate, m_parameters.channels);
        m_scaleData[fftSize] = std::make_shared<ScaleData>
            (guidedParameters, m_log);
    }

    m_calculator = std::unique_ptr<StretchCalculator>
        (new StretchCalculator(int(round(m_parameters.sampleRate)), //!!! which is a double...
                               1, false, // no fixed inputIncrement
                               m_log));

    if (isRealTime()) {
        createResampler();
        // In offline mode we don't create the resampler yet - we
        // don't want to have one at all if the pitch ratio is 1.0,
        // but that could change before the first process call, so we
        // create the resampler if needed then
    }

    calculateHop();

    m_prevInhop = m_inhop;
    m_prevOuthop = int(round(m_inhop * getEffectiveRatio()));

    if (!m_inhop.is_lock_free()) {
        m_log.log(0, "WARNING: std::atomic<int> is not lock-free");
    }
    if (!m_timeRatio.is_lock_free()) {
        m_log.log(0, "WARNING: std::atomic<double> is not lock-free");
    }
}

WindowType
R3Stretcher::ScaleData::analysisWindowShape(int fftSize)
{
    if (fftSize > 2048) return HannWindow;
    else return NiemitaloForwardWindow;
}

int
R3Stretcher::ScaleData::analysisWindowLength(int fftSize)
{
    return fftSize;
}

WindowType
R3Stretcher::ScaleData::synthesisWindowShape(int fftSize)
{
    if (fftSize > 2048) return HannWindow;
    else return NiemitaloReverseWindow;
}

int
R3Stretcher::ScaleData::synthesisWindowLength(int fftSize)
{
    if (fftSize > 2048) return fftSize/2;
    else return fftSize;
}

void
R3Stretcher::setTimeRatio(double ratio)
{
    if (!isRealTime()) {
        if (m_mode == ProcessMode::Studying ||
            m_mode == ProcessMode::Processing) {
            m_log.log(0, "R3Stretcher::setTimeRatio: Cannot set time ratio while studying or processing in non-RT mode");
            return;
        }
    }

    if (ratio == m_timeRatio) return;
    m_timeRatio = ratio;
    calculateHop();
}

void
R3Stretcher::setPitchScale(double scale)
{
    if (!isRealTime()) {
        if (m_mode == ProcessMode::Studying ||
            m_mode == ProcessMode::Processing) {
            m_log.log(0, "R3Stretcher::setTimeRatio: Cannot set pitch scale while studying or processing in non-RT mode");
            return;
        }
    }

    if (scale == m_pitchScale) return;
    m_pitchScale = scale;
    calculateHop();
}

void
R3Stretcher::setFormantScale(double scale)
{
    if (!isRealTime()) {
        if (m_mode == ProcessMode::Studying ||
            m_mode == ProcessMode::Processing) {
            m_log.log(0, "R3Stretcher::setTimeRatio: Cannot set formant scale while studying or processing in non-RT mode");
            return;
        }
    }

    m_formantScale = scale;
}

void
R3Stretcher::setFormantOption(RubberBandStretcher::Options options)
{
    int mask = (RubberBandStretcher::OptionFormantShifted |
                RubberBandStretcher::OptionFormantPreserved);
    m_parameters.options &= ~mask;
    options &= mask;
    m_parameters.options |= options;
}

void
R3Stretcher::setPitchOption(RubberBandStretcher::Options)
{
    m_log.log(0, "R3Stretcher::setPitchOption: Option change after construction is not supported in R3 engine");
}

void
R3Stretcher::setKeyFrameMap(const std::map<size_t, size_t> &mapping)
{
    if (isRealTime()) {
        m_log.log(0, "R3Stretcher::setKeyFrameMap: Cannot specify key frame map in RT mode");
        return;
    }
    if (m_mode == ProcessMode::Processing || m_mode == ProcessMode::Finished) {
        m_log.log(0, "R3Stretcher::setKeyFrameMap: Cannot specify key frame map after process() has begun");
        return;
    }

    m_keyFrameMap = mapping;
}

void
R3Stretcher::createResampler()
{
    Resampler::Parameters resamplerParameters;

    if (m_parameters.options & RubberBandStretcher::OptionPitchHighQuality) {
        resamplerParameters.quality = Resampler::Best;
    } else {
        resamplerParameters.quality = Resampler::FastestTolerable;
    }
    
    resamplerParameters.initialSampleRate = m_parameters.sampleRate;
    resamplerParameters.maxBufferSize = m_guideConfiguration.longestFftSize;

    if (isRealTime()) {
        if (m_parameters.options &
            RubberBandStretcher::OptionPitchHighConsistency) {
            resamplerParameters.dynamism = Resampler::RatioOftenChanging;
            resamplerParameters.ratioChange = Resampler::SmoothRatioChange;
        } else {
            resamplerParameters.dynamism = Resampler::RatioMostlyFixed;
            resamplerParameters.ratioChange = Resampler::SmoothRatioChange;
        }
    } else {
        resamplerParameters.dynamism = Resampler::RatioMostlyFixed;
        resamplerParameters.ratioChange = Resampler::SuddenRatioChange;
    }
    
    m_resampler = std::unique_ptr<Resampler>
        (new Resampler(resamplerParameters, m_parameters.channels));
}

void
R3Stretcher::calculateHop()
{
    double ratio = getEffectiveRatio();

    // In R2 we generally targeted a certain inhop, and calculated
    // outhop from that. Here we are the other way around. We aim for
    // outhop = 256 at ratios around 1, reducing down to 128 for
    // ratios far below 1 and up to 512 for ratios far above. As soon
    // as outhop exceeds 256 we have to drop the 1024-bin FFT, as the
    // overlap will be inadequate for it (that's among the jobs of the
    // Guide class) so we don't want to go above 256 until at least
    // factor 1.5. Also we can't go above 512 without changing the
    // window shape or dropping the 2048-bin FFT, and we can't do
    // either of those dynamically.

    double proposedOuthop = 256.0;
    if (ratio > 1.5) {
        proposedOuthop = pow(2.0, 8.0 + 2.0 * log10(ratio - 0.5));
    } else if (ratio < 1.0) {
        proposedOuthop = pow(2.0, 8.0 + 2.0 * log10(ratio));
    }        
    if (proposedOuthop > 512.0) proposedOuthop = 512.0;
    if (proposedOuthop < 128.0) proposedOuthop = 128.0;

    m_log.log(1, "calculateHop: ratio and proposed outhop", ratio, proposedOuthop);
    
    double inhop = proposedOuthop / ratio;
    if (inhop < 1.0) {
        m_log.log(0, "WARNING: Extreme ratio yields ideal inhop < 1, results may be suspect", ratio, inhop);
        inhop = 1.0;
    }
    if (inhop > 1024.0) {
        m_log.log(0, "WARNING: Extreme ratio yields ideal inhop > 1024, results may be suspect", ratio, inhop);
        inhop = 1024.0;
    }

    m_inhop = int(floor(inhop));

    m_log.log(1, "calculateHop: inhop and mean outhop", m_inhop, m_inhop * ratio);
}

void
R3Stretcher::updateRatioFromMap()
{
    if (m_keyFrameMap.empty()) return;

    if (m_consumedInputDuration == 0) {
        m_timeRatio = double(m_keyFrameMap.begin()->second) /
            double(m_keyFrameMap.begin()->first);

        m_log.log(1, "initial key-frame map entry ",
                   double(m_keyFrameMap.begin()->first),
                   double(m_keyFrameMap.begin()->second));
        m_log.log(1, "giving initial ratio ", m_timeRatio);
        
        calculateHop();
        m_lastKeyFrameSurpassed = 0;
        return;
    }
    
    auto i0 = m_keyFrameMap.upper_bound(m_lastKeyFrameSurpassed);

    if (i0 == m_keyFrameMap.end()) {
        return;
    }

    if (m_consumedInputDuration >= i0->first) {

        m_log.log(1, "input duration surpasses pending key frame",
                   double(m_consumedInputDuration), double(i0->first));
        
        auto i1 = m_keyFrameMap.upper_bound(m_consumedInputDuration);

        size_t keyFrameAtInput, keyFrameAtOutput;
    
        if (i1 != m_keyFrameMap.end()) {
            keyFrameAtInput = i1->first;
            keyFrameAtOutput = i1->second;
        } else {
            keyFrameAtInput = m_studyInputDuration;
            keyFrameAtOutput = m_totalTargetDuration;
        }

        m_log.log(1, "current input and output",
                   double(m_consumedInputDuration), double(m_totalOutputDuration));
        m_log.log(1, "next key frame input and output",
                   double(keyFrameAtInput), double(keyFrameAtOutput));
        
        double ratio;

        if (keyFrameAtInput > i0->first) {
        
            size_t toKeyFrameAtInput, toKeyFrameAtOutput;
            
            toKeyFrameAtInput = keyFrameAtInput - i0->first;
            
            if (keyFrameAtOutput > i0->second) {
                toKeyFrameAtOutput = keyFrameAtOutput - i0->second;
            } else {
                m_log.log(1, "previous target key frame overruns next key frame (or total output duration)", i0->second, keyFrameAtOutput);
                toKeyFrameAtOutput = 1;
            }

            m_log.log(1, "diff to next key frame input and output",
                      double(toKeyFrameAtInput), double(toKeyFrameAtOutput));

            ratio = double(toKeyFrameAtOutput) / double(toKeyFrameAtInput);

        } else {
            m_log.log(1, "source key frame overruns following key frame or total input duration", i0->first, keyFrameAtInput);
            ratio = 1.0;
        }
        
        m_log.log(1, "new ratio", ratio);
    
        m_timeRatio = ratio;
        calculateHop();

        m_lastKeyFrameSurpassed = i0->first;
    }
}

double
R3Stretcher::getTimeRatio() const
{
    return m_timeRatio;
}

double
R3Stretcher::getPitchScale() const
{
    return m_pitchScale;
}

double
R3Stretcher::getFormantScale() const
{
    return m_formantScale;
}

size_t
R3Stretcher::getPreferredStartPad() const
{
    if (!isRealTime()) {
        return 0;
    } else {
        return m_guideConfiguration.longestFftSize / 2;
    }
}

size_t
R3Stretcher::getStartDelay() const
{
    if (!isRealTime()) {
        return 0;
    } else {
        double factor = 0.5 / m_pitchScale;
        return size_t(ceil(m_guideConfiguration.longestFftSize * factor));
    }
}

size_t
R3Stretcher::getChannelCount() const
{
    return m_parameters.channels;
}

void
R3Stretcher::reset()
{
    m_calculator->reset();
    if (m_resampler) {
        m_resampler->reset();
    }

    for (auto &it : m_scaleData) {
        it.second->guided.reset();
    }

    for (auto &cd : m_channelData) {
        cd->reset();
    }

    m_prevInhop = m_inhop;
    m_prevOuthop = int(round(m_inhop * getEffectiveRatio()));

    m_studyInputDuration = 0;
    m_suppliedInputDuration = 0;
    m_totalTargetDuration = 0;
    m_consumedInputDuration = 0;
    m_lastKeyFrameSurpassed = 0;
    m_totalOutputDuration = 0;
    m_keyFrameMap.clear();

    m_mode = ProcessMode::JustCreated;
}

void
R3Stretcher::study(const float *const *, size_t samples, bool)
{
    if (isRealTime()) {
        m_log.log(0, "R3Stretcher::study: Not meaningful in realtime mode");
        return;
    }

    if (m_mode == ProcessMode::Processing || m_mode == ProcessMode::Finished) {
        m_log.log(0, "R3Stretcher::study: Cannot study after processing");
        return;
    }
    
    if (m_mode == ProcessMode::JustCreated) {
        m_studyInputDuration = 0;
    }

    m_mode = ProcessMode::Studying;
    m_studyInputDuration += samples;
}

void
R3Stretcher::setExpectedInputDuration(size_t samples)
{
    m_suppliedInputDuration = samples;
}

size_t
R3Stretcher::getSamplesRequired() const
{
    if (available() != 0) return 0;
    int longest = m_guideConfiguration.longestFftSize;
    int rs = m_channelData[0]->inbuf->getReadSpace();
    if (rs < longest) {
        return longest - rs;
    } else {
        return 0;
    }
}

void
R3Stretcher::setMaxProcessSize(size_t n)
{
    size_t oldSize = m_channelData[0]->inbuf->getSize();
    size_t newSize = m_guideConfiguration.longestFftSize + n;

    if (newSize > oldSize) {
        m_log.log(1, "setMaxProcessSize: resizing from and to", oldSize, newSize);
        for (int c = 0; c < m_parameters.channels; ++c) {
            m_channelData[c]->inbuf = std::unique_ptr<RingBuffer<float>>
                (m_channelData[c]->inbuf->resized(newSize));
        }
    } else {
        m_log.log(1, "setMaxProcessSize: nothing to be done, newSize <= oldSize", newSize, oldSize);
    }
}

void
R3Stretcher::process(const float *const *input, size_t samples, bool final)
{
    if (m_mode == ProcessMode::Finished) {
        m_log.log(0, "R3Stretcher::process: Cannot process again after final chunk");
        return;
    }

    if (!isRealTime()) {

        if (m_mode == ProcessMode::Studying) {
            m_totalTargetDuration =
                size_t(round(m_studyInputDuration * m_timeRatio));
            m_log.log(1, "study duration and target duration",
                      m_studyInputDuration, m_totalTargetDuration);
        } else if (m_mode == ProcessMode::JustCreated) {
            if (m_suppliedInputDuration != 0) {
                m_totalTargetDuration =
                    size_t(round(m_suppliedInputDuration * m_timeRatio));
                m_log.log(1, "supplied duration and target duration",
                          m_suppliedInputDuration, m_totalTargetDuration);
            }
        }

        // Update this on every process round, checking whether we've
        // surpassed the next key frame yet. This must follow the
        // overall target calculation above, which uses the "global"
        // time ratio, but precede any other use of the time ratio.
        
        if (!m_keyFrameMap.empty()) {
            updateRatioFromMap();
        }

        if (m_mode == ProcessMode::JustCreated ||
            m_mode == ProcessMode::Studying) {

            if (m_pitchScale != 1.0 && !m_resampler) {
                createResampler();
            }

            // Pad to half the longest frame. As with R2, in real-time
            // mode we don't do this -- it's better to start with a
            // swoosh than introduce more latency, and we don't want
            // gaps when the ratio changes.
            int pad = m_guideConfiguration.longestFftSize / 2;
            m_log.log(1, "offline mode: prefilling with", pad);
            for (int c = 0; c < m_parameters.channels; ++c) {
                m_channelData[c]->inbuf->zero(pad);
            }

            // NB by the time we skip this later we may have resampled
            // as well as stretched
            m_startSkip = int(round(pad / m_pitchScale));
            m_log.log(1, "start skip is", m_startSkip);
        }
    }

    if (final) {
        // We don't distinguish between Finished and "draining, but
        // haven't yet delivered all the samples" because the
        // distinction is meaningless internally - it only affects
        // whether available() finds any samples in the buffer
        m_mode = ProcessMode::Finished;
    } else {
        m_mode = ProcessMode::Processing;
    }
    
    size_t ws = m_channelData[0]->inbuf->getWriteSpace();
    if (samples > ws) {
        m_log.log(0, "R3Stretcher::process: WARNING: Forced to increase input buffer size. Either setMaxProcessSize was not properly called or process is being called repeatedly without retrieve. Write space and samples", ws, samples);
        size_t newSize = m_channelData[0]->inbuf->getSize() - ws + samples;
        for (int c = 0; c < m_parameters.channels; ++c) {
            auto newBuf = m_channelData[c]->inbuf->resized(newSize);
            m_channelData[c]->inbuf = std::unique_ptr<RingBuffer<float>>(newBuf);
        }
    }

    for (int c = 0; c < m_parameters.channels; ++c) {
        m_channelData[c]->inbuf->write(input[c], samples);
    }

    consume();
}

int
R3Stretcher::available() const
{
    int av = int(m_channelData[0]->outbuf->getReadSpace());
    if (av == 0 && m_mode == ProcessMode::Finished) {
        return -1;
    } else {
        return av;
    }
}

size_t
R3Stretcher::retrieve(float *const *output, size_t samples) const
{
    int got = samples;
    
    for (int c = 0; c < m_parameters.channels; ++c) {
        int gotHere = m_channelData[c]->outbuf->read(output[c], got);
        if (gotHere < got) {
            if (c > 0) {
                m_log.log(0, "R3Stretcher::retrieve: WARNING: channel imbalance detected");
            }
            got = std::min(got, std::max(gotHere, 0));
        }
    }

    return got;
}

void
R3Stretcher::consume()
{
    int longest = m_guideConfiguration.longestFftSize;
    int channels = m_parameters.channels;
    int inhop = m_inhop;

    double effectivePitchRatio = 1.0 / m_pitchScale;
    if (m_resampler) {
        effectivePitchRatio =
            m_resampler->getEffectiveRatio(effectivePitchRatio);
    }
    
    int outhop = m_calculator->calculateSingle(m_timeRatio,
                                               effectivePitchRatio,
                                               1.f,
                                               inhop,
                                               longest,
                                               longest,
                                               true);

    if (outhop < 1) {
        m_log.log(0, "R3Stretcher::consume: WARNING: outhop calculated as", outhop);
        outhop = 1;
    }
    
    // Now inhop is the distance by which the input stream will be
    // advanced after our current frame has been read, and outhop is
    // the distance by which the output will be advanced after it has
    // been emitted; m_prevInhop and m_prevOuthop are the
    // corresponding values the last time a frame was processed (*not*
    // just the last time this function was called, since we can
    // return without doing anything if the output buffer is full).
    //
    // Our phase adjustments need to be based on the distances we have
    // advanced the input and output since the previous frame, not the
    // distances we are about to advance them, so they use the m_prev
    // values.

    if (inhop != m_prevInhop) {
        m_log.log(2, "change in inhop", double(m_prevInhop), double(inhop));
    }
    if (outhop != m_prevOuthop) {
        m_log.log(2, "change in outhop", double(m_prevOuthop), double(outhop));
    }

    auto &cd0 = m_channelData.at(0);
    
    while (cd0->outbuf->getWriteSpace() >= outhop) {

        // NB our ChannelData, ScaleData, and ChannelScaleData maps
        // contain shared_ptrs; whenever we retain one of them in a
        // variable, we do so by reference to avoid copying the
        // shared_ptr (as that is not realtime safe). Same goes for
        // the map iterators

        int readSpace = cd0->inbuf->getReadSpace();
        if (readSpace < longest) {
            if (m_mode == ProcessMode::Finished) {
                if (readSpace == 0) {
                    int fill = cd0->scales.at(longest)->accumulatorFill;
                    if (fill == 0) {
                        break;
                    } else {
                        m_log.log(1, "finished reading input, but samples remaining in output accumulator", fill);
                    }
                }
            } else {
                // await more input
                break;
            }
        }

        // Analysis
        
        for (int c = 0; c < channels; ++c) {
            analyseChannel(c, inhop, m_prevInhop, m_prevOuthop);
        }

        // Phase update. This is synchronised across all channels
        
        for (auto &it : m_channelData[0]->scales) {
            int fftSize = it.first;
            for (int c = 0; c < channels; ++c) {
                auto &cd = m_channelData.at(c);
                auto &scale = cd->scales.at(fftSize);
                m_channelAssembly.mag[c] = scale->mag.data();
                m_channelAssembly.phase[c] = scale->phase.data();
                m_channelAssembly.prevMag[c] = scale->prevMag.data();
                m_channelAssembly.guidance[c] = &cd->guidance;
                m_channelAssembly.outPhase[c] = scale->advancedPhase.data();
            }
            m_scaleData.at(fftSize)->guided.advance
                (m_channelAssembly.outPhase.data(),
                 m_channelAssembly.mag.data(),
                 m_channelAssembly.phase.data(),
                 m_channelAssembly.prevMag.data(),
                 m_guideConfiguration,
                 m_channelAssembly.guidance.data(),
                 m_prevInhop,
                 m_prevOuthop);
        }

        for (int c = 0; c < channels; ++c) {
            adjustPreKick(c);
        }
        
        // Resynthesis
        
        for (int c = 0; c < channels; ++c) {
            synthesiseChannel(c, outhop, readSpace == 0);
        }
        
        // Resample

        bool resampling = false;
        if (m_resampler) {
            if (m_pitchScale != 1.0 ||
                (m_parameters.options &
                 RubberBandStretcher::OptionPitchHighConsistency)) {
                resampling = true;
            }
        }
        
        int resampledCount = 0;
        if (resampling) {
            for (int c = 0; c < channels; ++c) {
                auto &cd = m_channelData.at(c);
                m_channelAssembly.mixdown[c] = cd->mixdown.data();
                m_channelAssembly.resampled[c] = cd->resampled.data();
            }
            resampledCount = m_resampler->resample
                (m_channelAssembly.resampled.data(),
                 m_channelData[0]->resampled.size(),
                 m_channelAssembly.mixdown.data(),
                 outhop,
                 1.0 / m_pitchScale,
                 m_mode == ProcessMode::Finished && readSpace < inhop);
        }

        // Emit

        int writeCount = outhop;
        if (resampling) {
            writeCount = resampledCount;
        }
        if (!isRealTime()) {
            if (m_totalTargetDuration > 0 &&
                m_totalOutputDuration + writeCount > m_totalTargetDuration) {
                m_log.log(1, "writeCount would take output beyond target",
                          m_totalOutputDuration, m_totalTargetDuration);
                auto reduced = m_totalTargetDuration - m_totalOutputDuration;
                m_log.log(1, "reducing writeCount from and to", writeCount, reduced);
                writeCount = reduced;
            }
        }

        int advanceCount = inhop;
        if (advanceCount > readSpace) {
            // This should happen only when draining (Finished)
            if (m_mode != ProcessMode::Finished) {
                m_log.log(0, "WARNING: readSpace < inhop when processing is not yet finished", readSpace, inhop);
            }
            advanceCount = readSpace;
        }
        
        for (int c = 0; c < channels; ++c) {
            auto &cd = m_channelData.at(c);
            if (resampling) {
                cd->outbuf->write(cd->resampled.data(), writeCount);
            } else {
                cd->outbuf->write(cd->mixdown.data(), writeCount);
            }
            cd->inbuf->skip(advanceCount);
        }

        m_consumedInputDuration += advanceCount;
        m_totalOutputDuration += writeCount;
        
        if (m_startSkip > 0) {
            int rs = cd0->outbuf->getReadSpace();
            int toSkip = std::min(m_startSkip, rs);
            for (int c = 0; c < channels; ++c) {
                m_channelData.at(c)->outbuf->skip(toSkip);
            }
            m_startSkip -= toSkip;
            m_totalOutputDuration = rs - toSkip;
        }
        
        m_prevInhop = inhop;
        m_prevOuthop = outhop;
    }
}

void
R3Stretcher::analyseChannel(int c, int inhop, int prevInhop, int prevOuthop)
{
    int longest = m_guideConfiguration.longestFftSize;
    int classify = m_guideConfiguration.classificationFftSize;

    auto &cd = m_channelData.at(c);
    process_t *buf = cd->scales.at(longest)->timeDomain.data();

    int readSpace = cd->inbuf->getReadSpace();
    if (readSpace < longest) {
        cd->inbuf->peek(buf, readSpace);
        v_zero(buf + readSpace, longest - readSpace);
    } else {
        cd->inbuf->peek(buf, longest);
    }
    
    // We have a single unwindowed frame at the longest FFT size
    // ("scale"). Populate the shorter FFT sizes from the centre of
    // it, windowing as we copy. The classification scale is handled
    // separately because it has readahead, so skip it here as well as
    // the longest. (In practice this means we are probably only
    // populating one scale)

    for (auto &it: cd->scales) {
        int fftSize = it.first;
        if (fftSize == classify || fftSize == longest) continue;
        int offset = (longest - fftSize) / 2;
        m_scaleData.at(fftSize)->analysisWindow.cut
            (buf + offset, it.second->timeDomain.data());
    }

    // The classification scale has a one-hop readahead, so populate
    // the readahead from further down the long unwindowed frame.

    auto &classifyScale = cd->scales.at(classify);
    ClassificationReadaheadData &readahead = cd->readahead;

    m_scaleData.at(classify)->analysisWindow.cut
        (buf + (longest - classify) / 2 + inhop,
         readahead.timeDomain.data());

    // If inhop has changed since the previous frame, we'll have to
    // populate the classification scale (but for analysis/resynthesis
    // rather than classification) anew rather than reuse the previous
    // readahead. Pity...

    bool haveValidReadahead = cd->haveReadahead;
    if (inhop != prevInhop) haveValidReadahead = false;

    if (!haveValidReadahead) {
        m_scaleData.at(classify)->analysisWindow.cut
            (buf + (longest - classify) / 2,
             classifyScale->timeDomain.data());
    }
            
    // Finally window the longest scale
    m_scaleData.at(longest)->analysisWindow.cut(buf);

    // FFT shift, forward FFT, and carry out cartesian-polar
    // conversion for each FFT size.

    // For the classification scale we need magnitudes for the full
    // range (polar only in a subset) and we operate in the readahead,
    // pulling current values from the existing readahead (except
    // where the inhop has changed as above, in which case we need to
    // do both readahead and current)

    if (haveValidReadahead) {
        v_copy(classifyScale->mag.data(),
               readahead.mag.data(),
               classifyScale->bufSize);
        v_copy(classifyScale->phase.data(),
               readahead.phase.data(),
               classifyScale->bufSize);
    }

    v_fftshift(readahead.timeDomain.data(), classify);
    m_scaleData.at(classify)->fft.forward(readahead.timeDomain.data(),
                                          classifyScale->real.data(),
                                          classifyScale->imag.data());

    for (const auto &b : m_guideConfiguration.fftBandLimits) {
        if (b.fftSize == classify) {

            ToPolarSpec spec;
            spec.magFromBin = 0;
            spec.magBinCount = classify/2 + 1;
            spec.polarFromBin = b.b0min;
            spec.polarBinCount = b.b1max - b.b0min + 1;
            convertToPolar(readahead.mag.data(),
                           readahead.phase.data(),
                           classifyScale->real.data(),
                           classifyScale->imag.data(),
                           spec);
                    
            v_scale(classifyScale->mag.data(),
                    1.0 / double(classify),
                    classifyScale->mag.size());
            break;
        }
    }

    cd->haveReadahead = true;

    // For the others (and the classify as well, if the inhop has
    // changed or we haven't filled the readahead yet) we operate
    // directly in the scale data and restrict the range for
    // cartesian-polar conversion
            
    for (auto &it: cd->scales) {
        int fftSize = it.first;
        if (fftSize == classify && haveValidReadahead) {
            continue;
        }
        
        auto &scale = it.second;

        v_fftshift(scale->timeDomain.data(), fftSize);

        m_scaleData.at(fftSize)->fft.forward(scale->timeDomain.data(),
                                             scale->real.data(),
                                             scale->imag.data());

        for (const auto &b : m_guideConfiguration.fftBandLimits) {
            if (b.fftSize == fftSize) {

                ToPolarSpec spec;

                // For the classify scale we always want the full
                // range, as all the magnitudes (though not
                // necessarily all phases) are potentially relevant to
                // classification and formant analysis. But this case
                // here only happens if we don't haveValidReadahead -
                // the normal case is above and just copies from the
                // previous readahead.
                if (fftSize == classify) {
                    spec.magFromBin = 0;
                    spec.magBinCount = classify/2 + 1;
                    spec.polarFromBin = b.b0min;
                    spec.polarBinCount = b.b1max - b.b0min + 1;
                } else {
                    spec.magFromBin = b.b0min;
                    spec.magBinCount = b.b1max - b.b0min + 1;
                    spec.polarFromBin = spec.magFromBin;
                    spec.polarBinCount = spec.magBinCount;
                }

                convertToPolar(scale->mag.data(),
                               scale->phase.data(),
                               scale->real.data(),
                               scale->imag.data(),
                               spec);

                v_scale(scale->mag.data() + spec.magFromBin,
                        1.0 / double(fftSize),
                        spec.magBinCount);
                
                break;
            }
        }
    }

    if (m_parameters.options & RubberBandStretcher::OptionFormantPreserved) {
        analyseFormant(c);
        adjustFormant(c);
    }
        
    // Use the classification scale to get a bin segmentation and
    // calculate the adaptive frequency guide for this channel

    v_copy(cd->classification.data(), cd->nextClassification.data(),
           cd->classification.size());
    cd->classifier->classify(readahead.mag.data(),
                             cd->nextClassification.data());

    cd->prevSegmentation = cd->segmentation;
    cd->segmentation = cd->nextSegmentation;
    cd->nextSegmentation = cd->segmenter->segment(cd->nextClassification.data());
/*
    if (c == 0) {
        double pb = cd->nextSegmentation.percussiveBelow;
        double pa = cd->nextSegmentation.percussiveAbove;
        double ra = cd->nextSegmentation.residualAbove;
        int pbb = binForFrequency(pb, classify, m_parameters.sampleRate);
        int pab = binForFrequency(pa, classify, m_parameters.sampleRate);
        int rab = binForFrequency(ra, classify, m_parameters.sampleRate);
        std::cout << "pb = " << pb << ", pbb = " << pbb << std::endl;
        std::cout << "pa = " << pa << ", pab = " << pab << std::endl;
        std::cout << "ra = " << ra << ", rab = " << rab << std::endl;
        std::cout << "s:";
        for (int i = 0; i <= classify/2; ++i) {
            if (i > 0) std::cout << ",";
            if (i < pbb || (i >= pab && i <= rab)) {
                std::cout << "1";
            } else {
                std::cout << "0";
            }
        }
        std::cout << std::endl;
    }
*/

    double ratio = getEffectiveRatio();

    if (fabs(ratio - 1.0) < 1.0e-7) {
        ++m_unityCount;
    } else {
        m_unityCount = 0;
    }

    bool tighterChannelLock =
        m_parameters.options & RubberBandStretcher::OptionChannelsTogether;
    
    m_guide.updateGuidance(ratio,
                           prevOuthop,
                           classifyScale->mag.data(),
                           classifyScale->prevMag.data(),
                           cd->readahead.mag.data(),
                           cd->segmentation,
                           cd->prevSegmentation,
                           cd->nextSegmentation,
                           v_mean(classifyScale->mag.data() + 1, classify/2),
                           m_unityCount,
                           isRealTime(),
                           tighterChannelLock,
                           cd->guidance);
/*
    if (c == 0) {
        if (cd->guidance.kick.present) {
            std::cout << "k:2" << std::endl;
        } else if (cd->guidance.preKick.present) {
            std::cout << "k:1" << std::endl;
        } else {
            std::cout << "k:0" << std::endl;
        }
    }
*/
}

void
R3Stretcher::analyseFormant(int c)
{
    auto &cd = m_channelData.at(c);
    auto &f = *cd->formant;

    int fftSize = f.fftSize;
    int binCount = fftSize/2 + 1;
    
    auto &scale = cd->scales.at(fftSize);
    auto &scaleData = m_scaleData.at(fftSize);

    scaleData->fft.inverseCepstral(scale->mag.data(), f.cepstra.data());
    
    int cutoff = int(floor(m_parameters.sampleRate / 650.0));
    if (cutoff < 1) cutoff = 1;

    f.cepstra[0] /= 2.0;
    f.cepstra[cutoff-1] /= 2.0;
    for (int i = cutoff; i < fftSize; ++i) {
        f.cepstra[i] = 0.0;
    }
    v_scale(f.cepstra.data(), 1.0 / double(fftSize), cutoff);

    scaleData->fft.forward(f.cepstra.data(), f.envelope.data(), f.spare.data());

    v_exp(f.envelope.data(), binCount);
    v_square(f.envelope.data(), binCount);

    for (int i = 0; i < binCount; ++i) {
        if (f.envelope[i] > 1.0e10) f.envelope[i] = 1.0e10;
    }
}

void
R3Stretcher::adjustFormant(int c)
{
    auto &cd = m_channelData.at(c);
        
    for (auto &it : cd->scales) {
        
        int fftSize = it.first;
        auto &scale = it.second;

        int highBin = int(floor(fftSize * 10000.0 / m_parameters.sampleRate));
        process_t targetFactor = process_t(cd->formant->fftSize) / process_t(fftSize);
        process_t formantScale = m_formantScale;
        if (formantScale == 0.0) formantScale = 1.0 / m_pitchScale;
        process_t sourceFactor = targetFactor / formantScale;
        process_t maxRatio = 60.0;
        process_t minRatio = 1.0 / maxRatio;

        for (const auto &b : m_guideConfiguration.fftBandLimits) {
            if (b.fftSize != fftSize) continue;
            for (int i = b.b0min; i < b.b1max && i < highBin; ++i) {
                process_t source = cd->formant->envelopeAt(i * sourceFactor);
                process_t target = cd->formant->envelopeAt(i * targetFactor);
                if (target > 0.0) {
                    process_t ratio = source / target;
                    if (ratio < minRatio) ratio = minRatio;
                    if (ratio > maxRatio) ratio = maxRatio;
                    scale->mag[i] *= ratio;
                }
            }
        }
    }
}

void
R3Stretcher::adjustPreKick(int c)
{
    auto &cd = m_channelData.at(c);
    auto fftSize = cd->guidance.fftBands[0].fftSize;
    if (cd->guidance.preKick.present) {
        auto &scale = cd->scales.at(fftSize);
        int from = binForFrequency(cd->guidance.preKick.f0,
                                   fftSize, m_parameters.sampleRate);
        int to = binForFrequency(cd->guidance.preKick.f1,
                                 fftSize, m_parameters.sampleRate);
        for (int i = from; i <= to; ++i) {
            process_t diff = scale->mag[i] - scale->prevMag[i];
            if (diff > 0.0) {
                scale->pendingKick[i] = diff;
                scale->mag[i] -= diff;
            }
        }
    } else if (cd->guidance.kick.present) {
        auto &scale = cd->scales.at(fftSize);
        int from = binForFrequency(cd->guidance.preKick.f0,
                                   fftSize, m_parameters.sampleRate);
        int to = binForFrequency(cd->guidance.preKick.f1,
                                 fftSize, m_parameters.sampleRate);
        for (int i = from; i <= to; ++i) {
            scale->mag[i] += scale->pendingKick[i];
            scale->pendingKick[i] = 0.0;
        }
    }                
}

void
R3Stretcher::synthesiseChannel(int c, int outhop, bool draining)
{
    int longest = m_guideConfiguration.longestFftSize;

    auto &cd = m_channelData.at(c);
        
    for (const auto &band : cd->guidance.fftBands) {
        int fftSize = band.fftSize;
        auto &scale = cd->scales.at(fftSize);
        auto &scaleData = m_scaleData.at(fftSize);

        // copy to prevMag before filtering
        v_copy(scale->prevMag.data(),
               scale->mag.data(),
               scale->bufSize);

        process_t winscale = process_t(outhop) / scaleData->windowScaleFactor;

        // The frequency filter is applied naively in the frequency
        // domain. Aliasing is reduced by the shorter resynthesis
        // window. We resynthesise each scale individually, then sum -
        // it's easier to manage scaling for in situations with a
        // varying resynthesis hop
            
        int lowBin = binForFrequency(band.f0, fftSize, m_parameters.sampleRate);
        int highBin = binForFrequency(band.f1, fftSize, m_parameters.sampleRate);
        if (highBin % 2 == 0 && highBin > 0) --highBin;

        if (lowBin > 0) {
            v_zero(scale->real.data(), lowBin);
            v_zero(scale->imag.data(), lowBin);
        }

        v_scale(scale->mag.data() + lowBin, winscale, highBin - lowBin);

        v_polar_to_cartesian(scale->real.data() + lowBin,
                             scale->imag.data() + lowBin,
                             scale->mag.data() + lowBin,
                             scale->advancedPhase.data() + lowBin,
                             highBin - lowBin);
        
        if (highBin < scale->bufSize) {
            v_zero(scale->real.data() + highBin, scale->bufSize - highBin);
            v_zero(scale->imag.data() + highBin, scale->bufSize - highBin);
        }

        scaleData->fft.inverse(scale->real.data(),
                               scale->imag.data(),
                               scale->timeDomain.data());
        
        v_fftshift(scale->timeDomain.data(), fftSize);

        // Synthesis window may be shorter than analysis window, so
        // copy and cut only from the middle of the time-domain frame;
        // and the accumulator length always matches the longest FFT
        // size, so as to make mixing straightforward, so there is an
        // additional offset needed for the target
                
        int synthesisWindowSize = scaleData->synthesisWindow.getSize();
        int fromOffset = (fftSize - synthesisWindowSize) / 2;
        int toOffset = (longest - synthesisWindowSize) / 2;

        scaleData->synthesisWindow.cutAndAdd
            (scale->timeDomain.data() + fromOffset,
             scale->accumulator.data() + toOffset);
    }

    // Mix this channel and move the accumulator along
            
    float *mixptr = cd->mixdown.data();
    v_zero(mixptr, outhop);

    for (auto &it : cd->scales) {
        auto &scale = it.second;

        process_t *accptr = scale->accumulator.data();
        for (int i = 0; i < outhop; ++i) {
            mixptr[i] += float(accptr[i]);
        }

        int n = scale->accumulator.size() - outhop;
        v_move(accptr, accptr + outhop, n);
        v_zero(accptr + n, outhop);

        if (draining) {
            if (scale->accumulatorFill > outhop) {
                auto newFill = scale->accumulatorFill - outhop;
                m_log.log(2, "draining: reducing accumulatorFill from, to", scale->accumulatorFill, newFill);
                scale->accumulatorFill = newFill;
            } else {
                scale->accumulatorFill = 0;
            }
        } else {
            scale->accumulatorFill = scale->accumulator.size();
        }
    }
}

}

