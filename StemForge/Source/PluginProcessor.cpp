#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Helpers
//==============================================================================
namespace
{
    juce::File dataDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("StemForge");
    }

    const char* defaultBackendText = R"CFG(# StemForge backend commands.
# {input} and {outdir} are filled in by the plugin. Edit freely, no rebuild needed.
#
# separate = splits the song into stems (must write .wav files whose names contain
#            piano / drums / bass / vocals / guitar / other somewhere under {outdir})
# midi     = turns one stem wav into a .mid file inside {outdir}
#
separate=python -m demucs -n htdemucs_6s -o "{outdir}" "{input}"
midi=basic-pitch "{outdir}" "{input}"

# Example if Python lives in a venv:
# separate="C:\StemForge\venv\Scripts\python.exe" -m demucs -n htdemucs_6s -o "{outdir}" "{input}"
# midi="C:\StemForge\venv\Scripts\basic-pitch.exe" "{outdir}" "{input}"
)CFG";

    struct Backend { juce::String separateCmd, midiCmd; };

    Backend loadBackend()
    {
        auto f = dataDir().getChildFile ("backend.txt");
        if (! f.existsAsFile())
        {
            f.getParentDirectory().createDirectory();
            f.replaceWithText (defaultBackendText);
        }

        Backend b;
        for (auto line : juce::StringArray::fromLines (f.loadFileAsString()))
        {
            line = line.trim();
            if (line.isEmpty() || line.startsWithChar ('#')) continue;
            auto key = line.upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase();
            auto val = line.fromFirstOccurrenceOf ("=", false, false).trim();
            if (key == "separate") b.separateCmd = val;
            else if (key == "midi") b.midiCmd = val;
        }
        return b;
    }

    // Runs a command line, streams its output (to read a "NN%" progress and keep a log).
    bool runCommand (const juce::String& cmd,
                     const std::atomic<bool>& cancel,
                     const std::function<void (float)>& onProgress,
                     juce::String& log)
    {
        juce::ChildProcess cp;
        if (! cp.start (cmd))
        {
            log = "Could not start: " + cmd;
            return false;
        }

        char buf[2048];
        for (;;)
        {
            const int n = cp.readProcessOutput (buf, (int) sizeof (buf));
            if (n <= 0) break;

            auto chunk = juce::String::fromUTF8 (buf, n);
            log = (log + chunk).getLastCharacters (4000);

            const int pct = chunk.lastIndexOfChar ('%');
            if (pct >= 1 && onProgress)
            {
                int s = pct;
                while (s > 0 && juce::CharacterFunctions::isDigit (chunk[s - 1])) --s;
                if (s < pct)
                    onProgress (juce::jlimit (0.0f, 1.0f, chunk.substring (s, pct).getFloatValue() / 100.0f));
            }

            if (cancel.load()) { cp.kill(); return false; }
        }

        cp.waitForProcessToFinish (30000);
        return cp.getExitCode() == 0;
    }

    // Any audio file -> 44.1 kHz stereo 16-bit wav (what the separators like best)
    bool decodeToWav (const juce::File& song, const juce::File& wavOut)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (song));
        if (r == nullptr || r->lengthInSamples <= 0) return false;

        const int inN = (int) r->lengthInSamples;
        juce::AudioBuffer<float> in (2, inN);
        r->read (&in, 0, inN, 0, true, true);
        if (r->numChannels == 1)
            in.copyFrom (1, 0, in, 0, 0, inN);

        const double targetSR = 44100.0;
        juce::AudioBuffer<float> out;

        if (std::abs (r->sampleRate - targetSR) < 1.0)
        {
            out = std::move (in);
        }
        else
        {
            const double ratio = r->sampleRate / targetSR;
            const int outN = (int) std::ceil ((double) inN / ratio);
            out.setSize (2, outN);
            for (int c = 0; c < 2; ++c)
            {
                juce::LagrangeInterpolator li;
                li.process (ratio, in.getReadPointer (c), out.getWritePointer (c), outN);
            }
        }

        wavOut.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (wavOut.createOutputStream());
        if (os == nullptr) return false;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), targetSR, 2, 16, {}, 0));
        if (w == nullptr) return false;
        os.release();   // writer owns the stream now

        return w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    }

    std::vector<float> makePeaks (const juce::AudioBuffer<float>& b, int buckets = 900)
    {
        std::vector<float> p ((size_t) buckets, 0.0f);
        const int n = b.getNumSamples();
        if (n == 0) return p;

        for (int i = 0; i < buckets; ++i)
        {
            const int s = (int) ((juce::int64) n * i / buckets);
            const int e = (int) ((juce::int64) n * (i + 1) / buckets);
            float m = 0.0f;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                m = juce::jmax (m, b.getMagnitude (ch, s, juce::jmax (1, e - s)));
            p[(size_t) i] = m;
        }
        return p;
    }

    juce::File findInDir (const juce::File& dir, const juce::String& wildcard, const juce::String& mustContain)
    {
        if (! dir.isDirectory()) return {};
        for (auto& f : dir.findChildFiles (juce::File::findFiles, true, wildcard))
            if (mustContain.isEmpty() || f.getFileNameWithoutExtension().containsIgnoreCase (mustContain))
                return f;
        return {};
    }
}

//==============================================================================
// Construction
//==============================================================================
const juce::StringArray& StemForgeProcessor::getStemNames()
{
    static const juce::StringArray names { "piano", "drums", "bass", "vocals", "guitar", "other" };
    return names;
}

juce::AudioProcessorValueTreeState::ParameterLayout StemForgeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<AudioParameterInt> (ParameterID { "root", 1 }, "Root Note", 0, 127, 60));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "start", 1 }, "Start (s)",
                                                  NormalisableRange<float> (0.0f, 600.0f, 0.01f, 0.5f), 0.0f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "length", 1 }, "Length (s)",
                                                  NormalisableRange<float> (0.1f, 600.0f, 0.01f, 0.4f), 8.0f));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "loop", 1 }, "Loop", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Gain (dB)",
                                                  NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f));
    return l;
}

StemForgeProcessor::StemForgeProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout()),
      pool (1)
{
    rootP   = apvts.getRawParameterValue ("root");
    startP  = apvts.getRawParameterValue ("start");
    lengthP = apvts.getRawParameterValue ("length");
    loopP   = apvts.getRawParameterValue ("loop");
    gainP   = apvts.getRawParameterValue ("gain");
}

StemForgeProcessor::~StemForgeProcessor()
{
    cancel = true;
    pool.removeAllJobs (true, 15000);
}

bool StemForgeProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void StemForgeProcessor::prepareToPlay (double, int)
{
    for (auto& v : voices) v.adsr.reset();
}

juce::AudioProcessorEditor* StemForgeProcessor::createEditor()
{
    return new StemForgeEditor (*this);
}

//==============================================================================
// Thread-safe getters
//==============================================================================
juce::String StemForgeProcessor::getStatus() const   { juce::ScopedLock sl (metaLock); return status; }
juce::File StemForgeProcessor::getSongFile() const   { juce::ScopedLock sl (metaLock); return songFile; }
juce::File StemForgeProcessor::getMidiFile() const   { juce::ScopedLock sl (metaLock); return midiFile; }
juce::String StemForgeProcessor::getStemName() const { juce::ScopedLock sl (metaLock); return stemName; }

void StemForgeProcessor::setStatus (const juce::String& s, float prog)
{
    { juce::ScopedLock sl (metaLock); status = s; }
    progress = prog;
}

//==============================================================================
// Separation: song -> stems (cached) -> stem loaded into the sampler
//==============================================================================
void StemForgeProcessor::separateAndLoad (const juce::File& song, const juce::String& stem)
{
    if (busy.exchange (true)) return;

    {
        juce::ScopedLock sl (metaLock);
        songFile = song;
        stemName = stem;
        midiFile = juce::File();
    }
    setStatus ("Preparing...", -1.0f);

    juce::WeakReference<StemForgeProcessor> weak (this);

    pool.addJob ([this, weak, song, stem]
    {
        auto fail = [this] (const juce::String& msg)
        {
            setStatus (msg, 0.0f);
            busy = false;
        };

        const auto backend = loadBackend();
        const auto cache = dataDir().getChildFile ("cache")
                               .getChildFile (juce::String::toHexString (song.hashCode64())
                                              + "_" + juce::String (song.getSize()));
        cache.createDirectory();
        const auto sepDir = cache.getChildFile ("sep");

        auto stemWav = findInDir (sepDir, "*.wav", stem);

        if (! stemWav.existsAsFile())
        {
            const auto input = cache.getChildFile ("input.wav");

            if (! input.existsAsFile())
            {
                setStatus ("Decoding song...", -1.0f);
                if (! decodeToWav (song, input))
                {
                    input.deleteFile();
                    fail ("Could not read that audio file (try wav, mp3, flac, ogg)");
                    return;
                }
            }

            if (backend.separateCmd.isEmpty())
            {
                fail ("No 'separate=' line in " + dataDir().getChildFile ("backend.txt").getFullPathName());
                return;
            }

            setStatus ("Separating stems... first time takes a few minutes", 0.0f);
            sepDir.createDirectory();

            auto cmd = backend.separateCmd.replace ("{input}", input.getFullPathName())
                                          .replace ("{outdir}", sepDir.getFullPathName());

            juce::String log;
            runCommand (cmd, cancel, [this] (float p) { progress = p; }, log);
            dataDir().getChildFile ("last_log.txt").replaceWithText (cmd + "\n\n" + log);

            stemWav = findInDir (sepDir, "*.wav", stem);
            if (! stemWav.existsAsFile())
            {
                fail ("Separation failed. See " + dataDir().getChildFile ("last_log.txt").getFullPathName());
                return;
            }
        }

        setStatus ("Loading " + stem + "...", -1.0f);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (stemWav));
        if (r == nullptr || r->lengthInSamples <= 0) { fail ("Could not read the separated stem"); return; }

        auto data = std::make_shared<Loaded>();
        const int n = (int) r->lengthInSamples;
        data->buffer.setSize (2, n);
        r->read (&data->buffer, 0, n, 0, true, true);
        if (r->numChannels == 1) data->buffer.copyFrom (1, 0, data->buffer, 0, 0, n);
        data->sampleRate = r->sampleRate;
        data->peaks = makePeaks (data->buffer);
        data->file = stemWav;
        data->name = stem;

        juce::MessageManager::callAsync ([weak, data]
        {
            if (auto* p = weak.get())
            {
                p->installStem (data);
                p->setStatus ("Ready: " + data->name + ". Play it from the piano roll.", 0.0f);
                p->busy = false;
            }
        });
    });
}

// Re-load a stem wav we already have (used when FL reopens a project)
void StemForgeProcessor::loadStemFile (const juce::File& file, const juce::String& name)
{
    if (busy.exchange (true)) return;
    setStatus ("Loading " + name + "...", -1.0f);

    juce::WeakReference<StemForgeProcessor> weak (this);
    pool.addJob ([this, weak, file, name]
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));

        if (r == nullptr || r->lengthInSamples <= 0)
        {
            setStatus ("Could not reload the saved stem", 0.0f);
            busy = false;
            return;
        }

        auto data = std::make_shared<Loaded>();
        const int n = (int) r->lengthInSamples;
        data->buffer.setSize (2, n);
        r->read (&data->buffer, 0, n, 0, true, true);
        if (r->numChannels == 1) data->buffer.copyFrom (1, 0, data->buffer, 0, 0, n);
        data->sampleRate = r->sampleRate;
        data->peaks = makePeaks (data->buffer);
        data->file = file;
        data->name = name;

        juce::MessageManager::callAsync ([weak, data]
        {
            if (auto* p = weak.get())
            {
                p->installStem (data);
                p->setStatus ("Ready: " + data->name + ". Play it from the piano roll.", 0.0f);
                p->busy = false;
            }
        });
    });
}

void StemForgeProcessor::installStem (std::shared_ptr<Loaded> d)
{
    const double secs = d->buffer.getNumSamples() / d->sampleRate;
    {
        juce::SpinLock::ScopedLockType sl (bufferLock);
        stem = std::move (d->buffer);
        stemSR = d->sampleRate;
        for (auto& v : voices) v.adsr.reset();
    }
    peaks = std::move (d->peaks);
    stemSeconds = secs;

    juce::ScopedLock sl (metaLock);
    stemFile = d->file;
    stemName = d->name;
}

//==============================================================================
// MIDI export: stem wav -> .mid (polyphonic, via Basic Pitch)
//==============================================================================
void StemForgeProcessor::exportMidi()
{
    juce::File src;
    juce::String name;
    {
        juce::ScopedLock sl (metaLock);
        src = stemFile;
        name = stemName;
    }

    if (! src.existsAsFile()) { setStatus ("Load a stem first", 0.0f); return; }
    if (busy.exchange (true)) return;

    setStatus ("Converting " + name + " to MIDI...", -1.0f);
    juce::WeakReference<StemForgeProcessor> weak (this);

    pool.addJob ([this, weak, src, name]
    {
        const auto backend = loadBackend();
        const auto midiDir = src.getParentDirectory().getChildFile ("midi_" + name);
        midiDir.createDirectory();

        auto mid = findInDir (midiDir, "*.mid", {});

        if (! mid.existsAsFile())
        {
            if (backend.midiCmd.isEmpty())
            {
                setStatus ("No 'midi=' line in backend.txt", 0.0f);
                busy = false;
                return;
            }

            auto cmd = backend.midiCmd.replace ("{input}", src.getFullPathName())
                                      .replace ("{outdir}", midiDir.getFullPathName());
            juce::String log;
            runCommand (cmd, cancel, nullptr, log);
            dataDir().getChildFile ("last_log.txt").replaceWithText (cmd + "\n\n" + log);
            mid = findInDir (midiDir, "*.mid", {});
        }

        juce::MessageManager::callAsync ([weak, mid]
        {
            if (auto* p = weak.get())
            {
                if (mid.existsAsFile())
                {
                    { juce::ScopedLock sl (p->metaLock); p->midiFile = mid; }
                    p->setStatus ("MIDI ready. Drag it into the FL piano roll.", 0.0f);
                }
                else
                {
                    p->setStatus ("MIDI conversion failed. See last_log.txt in the StemForge folder", 0.0f);
                }
                p->busy = false;
            }
        });
    });
}

//==============================================================================
// Sampler engine
//==============================================================================
void StemForgeProcessor::noteOn (int note, float velocity)
{
    Voice* v = nullptr;
    for (auto& x : voices)
        if (! x.adsr.isActive()) { v = &x; break; }

    if (v == nullptr)   // steal the oldest
    {
        v = &voices[0];
        for (auto& x : voices) if (x.age < v->age) v = &x;
    }

    const double hostSR = getSampleRate();
    v->note = note;
    v->pos = 0.0;
    v->velocity = velocity;
    v->age = ++ageCounter;
    v->ratio = std::pow (2.0, (note - (int) rootP->load()) / 12.0) * stemSR / hostSR;
    v->adsr.setSampleRate (hostSR);
    v->adsr.setParameters ({ 0.003f, 0.0f, 1.0f, 0.08f });
    v->adsr.noteOn();
}

void StemForgeProcessor::noteOff (int note)
{
    for (auto& v : voices)
        if (v.note == note) v.adsr.noteOff();
}

void StemForgeProcessor::renderVoices (juce::AudioBuffer<float>& out, int start, int num)
{
    if (num <= 0) return;

    const int stemN = stem.getNumSamples();
    const int stemCh = stem.getNumChannels();
    const double regionStart = juce::jlimit (0.0, (double) stemN - 2.0, (double) startP->load() * stemSR);
    const double regionLen = juce::jmin ((double) lengthP->load() * stemSR, (double) stemN - regionStart - 1.0);
    if (regionLen < 2.0) return;

    const bool loop = loopP->load() > 0.5f;
    const float gain = juce::Decibels::decibelsToGain (gainP->load());

    const int nOut = juce::jmin (out.getNumChannels(), 2);
    float* dst[2] = { nullptr, nullptr };
    const float* src[2] = { nullptr, nullptr };
    for (int c = 0; c < nOut; ++c)
    {
        dst[c] = out.getWritePointer (c) + start;
        src[c] = stem.getReadPointer (juce::jmin (c, stemCh - 1));
    }

    for (auto& v : voices)
    {
        if (! v.adsr.isActive()) continue;

        for (int i = 0; i < num; ++i)
        {
            if (v.pos >= regionLen)
            {
                if (loop) v.pos = std::fmod (v.pos, regionLen);
                else { v.adsr.reset(); break; }
            }

            const double p = regionStart + v.pos;
            const int i0 = (int) p;
            const int i1 = juce::jmin (i0 + 1, stemN - 1);
            const float frac = (float) (p - i0);
            const float env = v.adsr.getNextSample() * v.velocity * gain;

            for (int c = 0; c < nOut; ++c)
                dst[c][i] += (src[c][i0] + frac * (src[c][i1] - src[c][i0])) * env;

            v.pos += v.ratio;
        }
    }
}

void StemForgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    juce::SpinLock::ScopedTryLockType lock (bufferLock);
    if (! lock.isLocked() || stem.getNumSamples() < 4)
        return;

    int pos = 0;
    for (const auto meta : midi)
    {
        const int evPos = juce::jlimit (pos, buffer.getNumSamples(), meta.samplePosition);
        renderVoices (buffer, pos, evPos - pos);
        pos = evPos;

        const auto m = meta.getMessage();
        if (m.isNoteOn())                               noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                         noteOff (m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) v.adsr.noteOff();
    }
    renderVoices (buffer, pos, buffer.getNumSamples() - pos);
}

//==============================================================================
// State (knobs + which stem file to reload with the FL project)
//==============================================================================
void StemForgeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    {
        juce::ScopedLock sl (metaLock);
        state.setProperty ("stemFile", stemFile.getFullPathName(), nullptr);
        state.setProperty ("stemName", stemName, nullptr);
        state.setProperty ("songFile", songFile.getFullPathName(), nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void StemForgeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (vt);

            const juce::File sf (vt["stemFile"].toString());
            const juce::File song (vt["songFile"].toString());
            const auto name = vt["stemName"].toString();

            {
                juce::ScopedLock sl (metaLock);
                if (name.isNotEmpty()) stemName = name;
                songFile = song;
            }

            if (sf.existsAsFile())
                loadStemFile (sf, name.isNotEmpty() ? name : juce::String ("piano"));
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StemForgeProcessor();
}
