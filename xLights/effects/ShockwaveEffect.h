#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/smeighan/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/smeighan/xLights/blob/master/License.txt
 **************************************************************/

#include "RenderableEffect.h"

#define SHOCKWAVE_X_MIN 0
#define SHOCKWAVE_X_MAX 100

#define SHOCKWAVE_Y_MIN 0
#define SHOCKWAVE_Y_MAX 100

#define SHOCKWAVE_STARTWIDTH_MIN 0
#define SHOCKWAVE_STARTWIDTH_MAX 255

#define SHOCKWAVE_ENDWIDTH_MIN 0
#define SHOCKWAVE_ENDWIDTH_MAX 255

#define SHOCKWAVE_STARTRADIUS_MIN 0
#define SHOCKWAVE_STARTRADIUS_MAX 750

#define SHOCKWAVE_ENDRADIUS_MIN 0
#define SHOCKWAVE_ENDRADIUS_MAX 750

class ShockwaveEffect : public RenderableEffect
{
    public:
        ShockwaveEffect(int id);
        virtual ~ShockwaveEffect();
        virtual void Render(Effect *effect, SettingsMap &settings, RenderBuffer &buffer) override;
        virtual int DrawEffectBackground(const Effect *e, int x1, int y1, int x2, int y2,
                                         xlVertexColorAccumulator &backgrounds, xlColor* colorMask, bool ramps) override;
        virtual void SetDefaultParameters() override;
        virtual bool SupportsRadialColorCurves(const SettingsMap &SettingsMap) const override { return true; }
        virtual bool CanRenderPartialTimeInterval() const override { return true; }
        virtual bool SupportsRenderCache(const SettingsMap& settings) const override { return true; }

    protected:
        virtual xlEffectPanel *CreatePanel(wxWindow *parent) override;
};
