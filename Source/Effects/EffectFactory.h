#pragma once

#include <array>
#include <memory>
#include <vector>

#include "Effects/Distortion/BitcrusherEffect.h"
#include "Effects/Distortion/DistortionEffect.h"
#include "Effects/Distortion/FuzzEffect.h"
#include "Effects/Distortion/OverdriveEffect.h"
#include "Effects/Dynamics/CompressorEffect.h"
#include "Effects/Dynamics/NoiseGateEffect.h"
#include "Effects/Filter/AutoWahEffect.h"
#include "Effects/Filter/EQ3BandEffect.h"
#include "Effects/Filter/WahEffect.h"
#include "Effects/Modulation/ChorusEffect.h"
#include "Effects/Modulation/FlangerEffect.h"
#include "Effects/Modulation/PhaserEffect.h"
#include "Effects/Modulation/TremoloEffect.h"
#include "Effects/Modulation/VibratoEffect.h"
#include "Effects/Pitch/OctaveEffect.h"
#include "Effects/Pitch/PitchShifterEffect.h"
#include "Effects/Special/CustomChainEffect.h"
#include "Effects/Special/LooperEffect.h"
#include "Effects/Special/PluginHostEffect.h"
#include "Effects/Special/RingModulatorEffect.h"
#include "Effects/Special/SolidBodyEffect.h"
#include "Effects/TimeSpace/CabinetEffect.h"
#include "Effects/TimeSpace/DelayEffect.h"
#include "Effects/TimeSpace/ReverbEffect.h"

/*==============================================================================
  EffectFactory

  Instantiates every effect exactly once, on the thread that constructs the
  engine, before any audio device is opened. The chain is then a list of stable
  pointers into this pool: reordering, adding and removing slots only republishes
  pointers, so the audio thread never creates or destroys an effect and never has
  to worry about a pointer becoming invalid underneath it.
==============================================================================*/
namespace pedal
{

struct EffectMetadata
{
    int type;
    const char* name;
    const char* category;
    const char* blurb;
};

class EffectFactory
{
public:
    explicit EffectFactory (const EffectContext& context);

    /** Stable for the lifetime of the process. */
    PedalEffect* get (EffectTypeID id) const noexcept
    {
        return instances[static_cast<std::size_t> (id)].get();
    }

    static const std::vector<EffectMetadata>& metadata();
    static const EffectMetadata* metadataFor (int type) noexcept;

    /** The order the app starts with, chosen so that a guitar straight into the
        chain is usable without touching anything: clean up first, then the
        always-on dynamics and EQ, then one drive and one time-based effect. */
    static const std::vector<EffectTypeID>& defaultChain();

    /** Categories in display order for the "add effect" menu. */
    static const std::vector<const char*>& categories();

private:
    std::array<std::unique_ptr<PedalEffect>, numEffectTypes> instances;
};

} // namespace pedal
