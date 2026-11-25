#pragma once
#include "PluginProcessor.h"

// Small box you grab and drag out into FL Studio (drops the .mid file)
class MidiDragZone : public juce::Component
{
public:
    void setFile (const juce::File& f)
    {
        if (f != file) { file = f; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const bool ok = file.existsAsFile();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (ok ? juce::Colour (0xff2e6f4e) : juce::Colour (0xff23262c));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (ok ? juce::Colours::white : juce::Colours::grey);
        g.drawFittedText (ok ? "Drag MIDI into FL piano roll" : "MIDI file appears here",
                          getLocalBounds().reduced (6, 0), juce::Justification::centred, 1);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (file.existsAsFile() && e.getDistanceFromDragStart() > 5)
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (file.existsAsFile()) file.revealToUser();
    }

private:
    juce::File file;
};

class StemForgeEditor : public juce::AudioProcessorEditor,
                        public juce::FileDragAndDropTarget,
                        private juce::Timer
{
public:
    explicit StemForgeEditor (StemForgeProcessor&);
    ~StemForgeEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragHover = false; repaint(); }

private:
    void timerCallback() override;

    StemForgeProcessor& proc;

    juce::ComboBox stemBox;
    juce::TextButton midiBtn { "Make MIDI" };
    MidiDragZone midiZone;
    juce::Label statusLabel;
    double progressValue = -1.0;
    juce::ProgressBar progressBar { progressValue };

    juce::Slider rootSlider, startSlider, lengthSlider, gainSlider;
    juce::ToggleButton loopButton { "Loop" };
    juce::Label rootLabel { {}, "Root note" }, startLabel { {}, "Start (s)" },
                lengthLabel { {}, "Length (s)" }, gainLabel { {}, "Gain (dB)" };

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> rootAtt, startAtt, lengthAtt, gainAtt;
    std::unique_ptr<ButtonAtt> loopAtt;

    juce::Rectangle<int> waveArea;
    bool dragHover = false;
    double lastSeconds = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemForgeEditor)
};
