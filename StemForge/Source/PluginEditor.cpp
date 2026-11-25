#include "PluginEditor.h"

StemForgeEditor::StemForgeEditor (StemForgeProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (640, 440);

    // stem picker
    const auto& names = StemForgeProcessor::getStemNames();
    for (int i = 0; i < names.size(); ++i)
        stemBox.addItem (names[i].substring (0, 1).toUpperCase() + names[i].substring (1), i + 1);
    stemBox.setSelectedId (juce::jmax (0, names.indexOf (proc.getStemName())) + 1, juce::dontSendNotification);
    stemBox.onChange = [this]
    {
        const auto song = proc.getSongFile();
        if (song.existsAsFile() && ! proc.isBusy())
            proc.separateAndLoad (song, StemForgeProcessor::getStemNames()[stemBox.getSelectedId() - 1]);
    };
    addAndMakeVisible (stemBox);

    midiBtn.onClick = [this] { proc.exportMidi(); };
    addAndMakeVisible (midiBtn);
    addAndMakeVisible (midiZone);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (statusLabel);
    addChildComponent (progressBar);

    auto setup = [this] (juce::Slider& s, juce::Label& l)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        s.onValueChange = [this] { repaint (waveArea); };
        l.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (s);
        addAndMakeVisible (l);
    };
    setup (rootSlider, rootLabel);
    setup (startSlider, startLabel);
    setup (lengthSlider, lengthLabel);
    setup (gainSlider, gainLabel);
    addAndMakeVisible (loopButton);

    rootAtt   = std::make_unique<SliderAtt> (proc.apvts, "root", rootSlider);
    startAtt  = std::make_unique<SliderAtt> (proc.apvts, "start", startSlider);
    lengthAtt = std::make_unique<SliderAtt> (proc.apvts, "length", lengthSlider);
    gainAtt   = std::make_unique<SliderAtt> (proc.apvts, "gain", gainSlider);
    loopAtt   = std::make_unique<ButtonAtt> (proc.apvts, "loop", loopButton);

    // show the root as a note name (set after the attachment, which installs its own text function)
    rootSlider.textFromValueFunction = [] (double v)
    {
        return juce::MidiMessage::getMidiNoteName ((int) v, true, true, 5);
    };
    rootSlider.updateText();

    startTimerHz (10);
}

bool StemForgeEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (proc.isBusy() || files.isEmpty()) return false;
    const auto ext = juce::File (files[0]).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" || ext == ".aif" || ext == ".aiff";
}

void StemForgeEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    if (files.isEmpty()) return;

    const auto stem = StemForgeProcessor::getStemNames()[juce::jmax (1, stemBox.getSelectedId()) - 1];
    proc.separateAndLoad (juce::File (files[0]), stem);
    repaint();
}

void StemForgeEditor::timerCallback()
{
    const bool busy = proc.isBusy();
    statusLabel.setText (proc.getStatus(), juce::dontSendNotification);
    progressValue = (double) proc.getProgress();
    progressBar.setVisible (busy);
    stemBox.setEnabled (! busy);
    midiBtn.setEnabled (! busy && proc.hasStem());
    midiZone.setFile (proc.getMidiFile());

    const auto secs = proc.getStemSeconds();
    if (secs != lastSeconds) { lastSeconds = secs; repaint(); }
}

void StemForgeEditor::resized()
{
    auto r = getLocalBounds().reduced (14);

    auto top = r.removeFromTop (32);
    top.removeFromLeft (150);   // title is painted
    stemBox.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (10);
    midiBtn.setBounds (top.removeFromLeft (100));
    top.removeFromLeft (10);
    midiZone.setBounds (top);

    r.removeFromTop (10);
    waveArea = r.removeFromTop (150);

    r.removeFromTop (12);
    auto row = [&r] (juce::Label& l, juce::Slider& s)
    {
        auto line = r.removeFromTop (30);
        l.setBounds (line.removeFromLeft (90));
        s.setBounds (line);
    };
    row (rootLabel, rootSlider);
    row (startLabel, startSlider);
    row (lengthLabel, lengthSlider);
    row (gainLabel, gainSlider);
    loopButton.setBounds (r.removeFromTop (28).removeFromLeft (100));

    r.removeFromTop (6);
    progressBar.setBounds (r.removeFromBottom (8));
    statusLabel.setBounds (r.removeFromBottom (24));
}

void StemForgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14161a));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("StemForge", 14, 14, 150, 32, juce::Justification::centredLeft);

    // waveform / drop area
    auto wa = waveArea.toFloat();
    g.setColour (dragHover ? juce::Colour (0xff2b3a52) : juce::Colour (0xff1c1f26));
    g.fillRoundedRectangle (wa, 8.0f);
    g.setColour (dragHover ? juce::Colour (0xff5b9bff) : juce::Colour (0xff343944));
    g.drawRoundedRectangle (wa, 8.0f, dragHover ? 2.0f : 1.0f);

    const auto& peaks = proc.getPeaks();
    const double secs = proc.getStemSeconds();

    if (peaks.empty() || secs <= 0.0)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText ("Drop a song here (wav, mp3, flac, ogg)", waveArea, juce::Justification::centred);
        return;
    }

    // played region (Start / Length)
    const float s0 = (float) (proc.apvts.getRawParameterValue ("start")->load() / secs);
    const float l0 = (float) (proc.apvts.getRawParameterValue ("length")->load() / secs);
    const float x0 = wa.getX() + juce::jlimit (0.0f, 1.0f, s0) * wa.getWidth();
    const float x1 = wa.getX() + juce::jlimit (0.0f, 1.0f, s0 + l0) * wa.getWidth();
    g.setColour (juce::Colour (0x333d8bff));
    g.fillRect (juce::Rectangle<float> (x0, wa.getY() + 1, juce::jmax (1.0f, x1 - x0), wa.getHeight() - 2));

    // peaks
    const float midY = wa.getCentreY();
    const float half = wa.getHeight() * 0.45f;
    g.setColour (juce::Colour (0xff7fb2ff));
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        const float x = wa.getX() + wa.getWidth() * (float) i / (float) peaks.size();
        const float h = juce::jlimit (1.0f, half, peaks[i] * half);
        g.drawLine (x, midY - h, x, midY + h);
    }
}
