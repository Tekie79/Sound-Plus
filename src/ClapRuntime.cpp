#include "ClapRuntime.h"
#include "Performance.h"
#include <juce_dsp/juce_dsp.h>
#include <onnxruntime_cxx_api.h>
#include <array>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace sp
{
namespace
{
std::vector<float> normalize(Ort::Value &value)
{
    auto count = value.GetTensorTypeAndShapeInfo().GetElementCount();
    if (count != 512) throw std::runtime_error("Unexpected CLAP embedding dimensions");
    auto *data = value.GetTensorMutableData<float>();
    std::vector<float> v(data, data + count);
    double sum = 0;
    for (auto x : v) { if (!std::isfinite(x)) throw std::runtime_error("Nonfinite CLAP output"); sum += x*x; }
    if (sum <= 1.0e-20) throw std::runtime_error("Empty CLAP embedding");
    for (auto &x : v) x = (float)(x / std::sqrt(sum));
    return v;
}
double mel(double hz) { return hz < 1000 ? 3*hz/200 : 15 + std::log(hz/1000)*27/std::log(6.4); }
double hertz(double m) { return m < 15 ? m*200/3 : 1000*std::exp((m-15)*std::log(6.4)/27); }
// Fixed 32-tap, 1024-phase low-pass resampler. Window positions remain in the source's sample space.
std::vector<float> resample(const std::vector<float> &in, double sourceRate)
{
    if (sourceRate == 48000) return in;
    auto size = (size_t)std::ceil(in.size()*48000/sourceRate);
    size = std::min<size_t>(480000, size);
    std::vector<float> out(size);
    constexpr int taps = 32, phases = 1024;
    std::vector<float> coefficients(taps*phases);
    auto cutoff = std::min(1.0, 48000/sourceRate)*0.95;
    const double pi = juce::MathConstants<double>::pi;
    for (int p = 0; p < phases; ++p)
    {
        double sum = 0;
        for (int k = 0; k < taps; ++k)
        {
            double x = k-15-(double)p/phases;
            double sinc = std::abs(x) < 1e-12 ? cutoff : std::sin(pi*x*cutoff)/(pi*x);
            double w = 0.42-0.5*std::cos(2*pi*k/(taps-1))+0.08*std::cos(4*pi*k/(taps-1));
            coefficients[(size_t)p*taps+k] = (float)(sinc*w); sum += sinc*w;
        }
        for (int k = 0; k < taps; ++k) coefficients[(size_t)p*taps+k] /= (float)sum;
    }
    for (size_t n = 0; n < size; ++n)
    {
        double position = n*sourceRate/48000;
        auto at = (int64_t)std::floor(position);
        int phase = std::min(phases-1, (int)((position-at)*phases));
        double value = 0;
        for (int k = 0; k < taps; ++k)
        {
            int64_t i = at+k-15;
            if (i >= 0 && i < (int64_t)in.size()) value += in[(size_t)i]*coefficients[(size_t)phase*taps+k];
        }
        out[n] = (float)value;
    }
    return out;
}
}
struct ClapRuntime::Impl
{
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR, "SoundPlus local CLAP"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> text, audio;
    std::unordered_map<std::string,int64_t> vocabulary;
    std::unordered_map<std::string,int> ranks;
    std::array<juce::String,256> byteEncoder;
    explicit Impl(const juce::File &folder)
    {
        environment.DisableTelemetryEvents();
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.AddConfigEntry("session.inter_op.allow_spinning", "0");
        text = std::make_unique<Ort::Session>(environment,
            folder.getChildFile("text_model_quantized.onnx").getFullPathName().toRawUTF8(), options);
        audio = std::make_unique<Ort::Session>(environment,
            folder.getChildFile("audio_model_quantized.onnx").getFullPathName().toRawUTF8(), options);
        if (text->GetInputCount()!=1 || audio->GetInputCount()!=1)
            throw std::runtime_error("Unsupported paired CLAP graph contract");
        auto tokenizer = juce::JSON::parse(folder.getChildFile("tokenizer.json"));
        if (auto *vocab = tokenizer["model"]["vocab"].getDynamicObject())
            for (auto &item : vocab->getProperties()) vocabulary[item.name.toString().toStdString()] = (int64_t)item.value;
        if (auto *merges = tokenizer["model"]["merges"].getArray())
            for (int n = 0; n < merges->size(); ++n) ranks[(*merges)[n].toString().toStdString()] = n;
        if (vocabulary.size() != 50265 || ranks.empty()) throw std::runtime_error("Invalid CLAP tokenizer");
        int next = 256;
        for (int b = 0; b < 256; ++b)
        {
            bool literal = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || b >= 174;
            byteEncoder[(size_t)b] = juce::String::charToString((juce::juce_wchar)(literal ? b : next++));
        }
    }
    void encodePart(const juce::String &part, std::vector<int64_t> &ids)
    {
        for (auto &piece : robertaPieces(part))
        {
            auto utf8 = piece.toStdString();
            std::vector<juce::String> symbols;
            for (unsigned char b : utf8) symbols.push_back(byteEncoder[b]);
            while (symbols.size() > 1)
            {
                int best = std::numeric_limits<int>::max(), at = -1;
                for (size_t n = 0; n+1 < symbols.size(); ++n)
                {
                    auto found = ranks.find((symbols[n]+" "+symbols[n+1]).toStdString());
                    if (found != ranks.end() && found->second < best) { best = found->second; at = (int)n; }
                }
                if (at < 0) break;
                auto left = symbols[(size_t)at], right = symbols[(size_t)at+1];
                std::vector<juce::String> merged;
                for (size_t n = 0; n < symbols.size(); ++n)
                    if (n+1 < symbols.size() && symbols[n] == left && symbols[n+1] == right)
                    { merged.push_back(symbols[n]+symbols[n+1]); ++n; }
                    else merged.push_back(symbols[n]);
                symbols = std::move(merged);
            }
            for (auto &symbol : symbols)
            {
                auto found = vocabulary.find(symbol.toStdString());
                ids.push_back(found == vocabulary.end() ? 3 : found->second);
                if (ids.size() >= 76) return;
            }
        }
    }
};
ClapRuntime::ClapRuntime(const juce::File &folder) : impl(std::make_unique<Impl>(folder)) {}
ClapRuntime::~ClapRuntime() = default;
std::vector<int64_t> ClapRuntime::tokenize(const juce::String &query)
{
    std::vector<int64_t> ids{0};
    // Recognize RoBERTa added tokens before byte-level pre-tokenization.
    juce::String remaining = query.substring(0, 8192);
    const std::array<juce::String,5> specials{"<s>","<pad>","</s>","<unk>","<mask>"};
    while (remaining.isNotEmpty() && ids.size() < 76)
    {
        int at = remaining.length(), token = -1;
        for (size_t n = 0; n < specials.size(); ++n)
        {
            int p = remaining.indexOf(specials[n]);
            if (p >= 0 && p < at) { at = p; token = (int)n; }
        }
        auto part = remaining.substring(0, at);
        if (token == 4) part = part.trimEnd();
        impl->encodePart(part, ids);
        if (token < 0 || ids.size() >= 76) break;
        ids.push_back(token == 4 ? 50264 : token);
        remaining = remaining.substring(at + specials[(size_t)token].length());
    }
    ids.resize(std::min<size_t>(76,ids.size())); ids.push_back(2);
    return ids;
}
std::vector<float> ClapRuntime::text(const juce::String &query)
{
    auto begin = monotonicMs();
    auto ids = tokenize(query);
    std::vector<int64_t> mask(ids.size(),1);
    std::array<int64_t,2> shape{1,(int64_t)ids.size()};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    std::array<Ort::Value,2> inputs{
        Ort::Value::CreateTensor<int64_t>(memory,ids.data(),ids.size(),shape.data(),shape.size()),
        Ort::Value::CreateTensor<int64_t>(memory,mask.data(),mask.size(),shape.data(),shape.size())};
    lastPreprocessMs = monotonicMs()-begin;
    const char *names[]{"input_ids","attention_mask"}; const char *outputs[]{"text_embeds"};
    auto before = monotonicMs();
    // This pinned export folds the all-ones mask and accepts input_ids only.
    // Variable-length, unpadded input preserves the upstream all-ones attention semantics.
    auto results = impl->text->Run(Ort::RunOptions{nullptr},names,inputs.data(),impl->text->GetInputCount(),outputs,1);
    lastInferenceMs = monotonicMs()-before;
    return normalize(results[0]);
}
std::vector<float> ClapRuntime::features(const std::vector<float> &input)
{
    if (input.empty() || input.size() > 480000) throw std::runtime_error("CLAP window must contain 1..480000 samples");
    std::vector<float> padded(480000);
    size_t repeated = (480000/input.size())*input.size();
    for (size_t n = 0; n < repeated; ++n) padded[n] = input[n%input.size()];
    std::array<std::vector<std::pair<int,float>>,64> filters;
    std::array<double,66> edges{};
    for (int n = 0; n < 66; ++n) edges[(size_t)n] = hertz(mel(50)+(mel(14000)-mel(50))*n/65);
    for (int n = 0; n < 64; ++n)
        for (int k = 0; k < 513; ++k)
        {
            double hz = k*48000.0/1024;
            double weight = std::max(0.0,std::min((hz-edges[(size_t)n])/(edges[(size_t)n+1]-edges[(size_t)n]),
                (edges[(size_t)n+2]-hz)/(edges[(size_t)n+2]-edges[(size_t)n+1])));
            weight *= 2/(edges[(size_t)n+2]-edges[(size_t)n]);
            if (weight > 0) filters[(size_t)n].emplace_back(k,(float)weight);
        }
    std::array<float,1024> window{};
    for (int n = 0; n < 1024; ++n) window[(size_t)n] = (float)(0.5-0.5*std::cos(2*juce::MathConstants<double>::pi*n/1024));
    juce::dsp::FFT fft(10);
    std::array<juce::dsp::Complex<float>,1024> in{},out{};
    std::array<float,513> power{};
    std::vector<float> result(1001*64);
    for (int frame = 0; frame < 1001; ++frame)
    {
        for (int n = 0; n < 1024; ++n)
        {
            int index = frame*480+n-512;
            if (index < 0) index = -index;
            if (index >= 480000) index = 960000-index-2;
            in[(size_t)n] = {padded[(size_t)index]*window[(size_t)n],0};
        }
        fft.perform(in.data(),out.data(),false);
        for (int k = 0; k < 513; ++k) power[(size_t)k] = std::norm(out[(size_t)k]);
        for (int band = 0; band < 64; ++band)
        {
            double sum = 0;
            for (auto [index,weight] : filters[(size_t)band]) sum += power[(size_t)index]*weight;
            result[(size_t)frame*64+band] = (float)(10*std::log10(std::max(1e-10,sum)));
        }
    }
    return result;
}
std::vector<float> ClapRuntime::audio(const juce::File &file, int64_t start, int64_t end,
                                     double expectedRate, int64_t expectedLength)
{
    auto begin = monotonicMs();
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->sampleRate <= 0 || reader->sampleRate > 384000 || reader->numChannels == 0 || reader->numChannels > 64)
        throw std::runtime_error("Unsupported or unreadable audio");
    if (reader->sampleRate != expectedRate || reader->lengthInSamples != expectedLength)
        throw std::runtime_error("Source changed before analysis");
    if (start < 0 || end <= start || end > expectedLength || end-start > (int64_t)std::ceil(expectedRate*10))
        throw std::runtime_error("Invalid source analysis interval");
    std::vector<float> mono((size_t)(end-start));
    juce::AudioBuffer<float> block((int)reader->numChannels,65536);
    for (int64_t at = start; at < end; at += block.getNumSamples())
    {
        int n = (int)std::min<int64_t>(block.getNumSamples(),end-at);
        if (!reader->read(&block,0,n,at,true,true)) throw std::runtime_error("Audio decoding failed");
        for (int j = 0; j < n; ++j)
        {
            double value = 0;
            for (int c = 0; c < block.getNumChannels(); ++c) value += block.getSample(c,j);
            if (!std::isfinite(value)) throw std::runtime_error("Nonfinite source audio; retry or replace this recording");
            mono[(size_t)(at-start+j)] = (float)(value/block.getNumChannels());
        }
    }
    double energy=0;for (float value:mono) energy+=(double)value*value;
    lastAnalysisRms=std::sqrt(energy/std::max<size_t>(1,mono.size()));
    auto data = features(resample(mono,expectedRate));
    lastPreprocessMs = monotonicMs()-begin;
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    std::array<int64_t,4> shape{1,1,1001,64};
    auto tensor = Ort::Value::CreateTensor<float>(memory,data.data(),data.size(),shape.data(),shape.size());
    const char *names[]{"input_features"}; const char *outputs[]{"audio_embeds"};
    auto before = monotonicMs();
    auto results = impl->audio->Run(Ort::RunOptions{nullptr},names,&tensor,1,outputs,1);
    lastInferenceMs = monotonicMs()-before;
    return normalize(results[0]);
}
} // namespace sp
