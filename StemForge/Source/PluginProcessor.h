#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <vector>

class StemForgeProcessor : public juce::AudioProcessor
{
public:
    StemForgeProcessor();
    ~StemForgeProcessor() override;

    // ---- AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "StemForge"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- UI API (call from the message thread) ----
    void separateAndLoad (const juce::File& song, const juce::String& stem);
    void exportMidi();

    juce::String getStatus() const;
    float getProgress() const { return progress.load(); }   // <0 = unknown
    bool isBusy() const { return busy.load(); }
    bool hasStem() const { return stemSeconds.load() > 0.0; }
    double getStemSeconds() const { return stemSeconds.load(); }
    juce::File getSongFile() const;
    juce::File getMidiFile() const;
    juce::String getStemName() const;
    const std::vector<float>& getPeaks() const { return peaks; }   // message thread only

    static const juce::StringArray& getStemNames();

    juce::AudioProcessorValueTreeState apvts;

private:
    struct Voice
    {
        juce::ADSR adsr;
        int note = 0;
        double pos = 0.0;      // position inside the region, in stem samples
        double ratio = 1.0;    // playback rate (pitch)
        float velocity = 1.0f;
        juce::uint32 age = 0;
    };

    struct Loaded
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 44100.0;
        std::vector<float> peaks;
        juce::File file;
        juce::String name;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void loadStemFile (const juce::File& file, const juce::String& name);
    void installStem (std::shared_ptr<Loaded> data);
    void setStatus (const juce::String& s, float prog);
    void noteOn (int note, float velocity);
    void noteOff (int note);
    void renderVoices (juce::AudioBuffer<float>& out, int start, int num);

    // sampler data (guarded by bufferLock)
    juce::SpinLock bufferLock;
    juce::AudioBuffer<float> stem;
    double stemSR = 44100.0;
    std::array<Voice, 16> voices;
    juce::uint32 ageCounter = 0;

    std::atomic<float>* rootP = nullptr;
    std::atomic<float>* startP = nullptr;
    std::atomic<float>* lengthP = nullptr;
    std::atomic<float>* loopP = nullptr;
    std::atomic<float>* gainP = nullptr;

    // background job state
    std::atomic<bool> busy { false };
    std::atomic<bool> cancel { false };
    std::atomic<float> progress { 0.0f };
    std::atomic<double> stemSeconds { 0.0 };

    mutable juce::CriticalSection metaLock;
    juce::String status { "Drop a song onto the window" };
    juce::File songFile, stemFile, midiFile;
    juce::String stemName { "piano" };

    std::vector<float> peaks;   // message thread only

    juce::ThreadPool pool;

    JUCE_DECLARE_WEAK_REFERENCEABLE (StemForgeProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemForgeProcessor)
};
