#pragma once
#include "DesignModel.h"
#include <juce_audio_utils/juce_audio_utils.h>
namespace sp
{
// Owned by a decoder/render worker, never by the real-time callback.
class DesignGraph
{
  public:
    explicit DesignGraph(const DesignDocument &);
    ~DesignGraph();
    void render(juce::AudioBuffer<float> &,int offset,int frames);
    void seek(int64_t frame,const std::function<bool()> &cancelled=[] {return false;});
    int layers() const;
    int64_t position() const;
    double rate() const;
  private:
    struct Impl;std::unique_ptr<Impl> impl;
};
class DesignReader
{
  public:
    DesignReader(const DesignDocument &,double outputRate,int64_t first=0,int64_t last=-1,double tailSeconds=0);
    ~DesignReader();
    int read(juce::AudioBuffer<float> &,int frames,const std::function<bool()> &cancelled=[] {return false;});
    int64_t length() const;
    int64_t consumed() const;
    int64_t endFrame() const;
    int layers() const;
  private:
    struct Impl;std::unique_ptr<Impl> impl;
};
DesignOutput renderDesign(const DesignDocument &,const juce::File &folder,const DesignRenderSettings &,
                          const std::function<bool()> &cancelled,const std::function<void(double)> &progress);
DesignOutput copyDesignOriginal(const DesignLayer &,const juce::File &folder,int64_t revision,
                                const std::function<bool()> &cancelled);
}
