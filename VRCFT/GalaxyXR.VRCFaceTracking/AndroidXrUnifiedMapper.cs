using VRCFaceTracking;
using VRCFaceTracking.Core.Params.Expressions;
using VRCFaceTracking.Core.Types;

namespace GalaxyXR.VRCFaceTracking;

internal static class AndroidXrUnifiedMapper
{
    internal static void Clear()
    {
        for (var index = 0; index < (int)UnifiedExpressions.Max; index++)
            UnifiedTracking.Data.Shapes[index].Weight = 0.0f;
        UnifiedTracking.Data.Eye.Left.Gaze = Vector2.zero;
        UnifiedTracking.Data.Eye.Right.Gaze = Vector2.zero;
        UnifiedTracking.Data.Eye.Left.Openness = 1.0f;
        UnifiedTracking.Data.Eye.Right.Openness = 1.0f;
    }

    internal static void Apply(float[] w)
    {
        Clear();
        Set(UnifiedExpressions.BrowLowererLeft, w[0]);
        Set(UnifiedExpressions.BrowLowererRight, w[1]);
        Set(UnifiedExpressions.CheekPuffLeft, w[2]);
        Set(UnifiedExpressions.CheekPuffRight, w[3]);
        Set(UnifiedExpressions.CheekSquintLeft, w[4]);
        Set(UnifiedExpressions.CheekSquintRight, w[5]);
        Set(UnifiedExpressions.CheekSuckLeft, w[6]);
        Set(UnifiedExpressions.CheekSuckRight, w[7]);
        Set(UnifiedExpressions.MouthRaiserLower, w[8]);
        Set(UnifiedExpressions.MouthRaiserUpper, w[9]);
        Set(UnifiedExpressions.MouthDimpleLeft, w[10]);
        Set(UnifiedExpressions.MouthDimpleRight, w[11]);

        UnifiedTracking.Data.Eye.Left.Openness = 1.0f - w[12];
        UnifiedTracking.Data.Eye.Right.Openness = 1.0f - w[13];
        UnifiedTracking.Data.Eye.Left.Gaze = new Vector2(w[18] - w[16], w[20] - w[14]);
        UnifiedTracking.Data.Eye.Right.Gaze = new Vector2(w[19] - w[17], w[21] - w[15]);

        Set(UnifiedExpressions.BrowInnerUpLeft, w[22]);
        Set(UnifiedExpressions.BrowInnerUpRight, w[23]);
        Set(UnifiedExpressions.JawOpen, w[24]);
        Set(UnifiedExpressions.JawLeft, w[25]);
        Set(UnifiedExpressions.JawRight, w[26]);
        Set(UnifiedExpressions.JawForward, w[27]);
        SetMax(UnifiedExpressions.EyeSquintLeft, w[28]);
        SetMax(UnifiedExpressions.EyeSquintRight, w[29]);
        Set(UnifiedExpressions.MouthFrownLeft, w[30]);
        Set(UnifiedExpressions.MouthFrownRight, w[31]);
        Set(UnifiedExpressions.MouthCornerPullLeft, w[32]);
        Set(UnifiedExpressions.MouthCornerPullRight, w[33]);
        Set(UnifiedExpressions.LipFunnelLowerLeft, w[34]);
        Set(UnifiedExpressions.LipFunnelUpperLeft, w[35]);
        Set(UnifiedExpressions.LipFunnelLowerRight, w[36]);
        Set(UnifiedExpressions.LipFunnelUpperRight, w[37]);
        Set(UnifiedExpressions.MouthPressLeft, w[38]);
        Set(UnifiedExpressions.MouthPressRight, w[39]);
        Set(UnifiedExpressions.LipPuckerUpperLeft, w[40]);
        Set(UnifiedExpressions.LipPuckerLowerLeft, w[40]);
        Set(UnifiedExpressions.LipPuckerUpperRight, w[41]);
        Set(UnifiedExpressions.LipPuckerLowerRight, w[41]);
        Set(UnifiedExpressions.MouthStretchLeft, w[42]);
        Set(UnifiedExpressions.MouthStretchRight, w[43]);
        Set(UnifiedExpressions.LipSuckLowerLeft, w[44]);
        Set(UnifiedExpressions.LipSuckUpperLeft, w[45]);
        Set(UnifiedExpressions.LipSuckLowerRight, w[46]);
        Set(UnifiedExpressions.LipSuckUpperRight, w[47]);
        Set(UnifiedExpressions.MouthTightenerLeft, w[48]);
        Set(UnifiedExpressions.MouthTightenerRight, w[49]);
        Set(UnifiedExpressions.MouthClosed, w[50]);
        Set(UnifiedExpressions.MouthLowerDownLeft, w[51]);
        Set(UnifiedExpressions.MouthLowerDownRight, w[52]);
        Set(UnifiedExpressions.MouthUpperLeft, w[53]);
        Set(UnifiedExpressions.MouthLowerLeft, w[53]);
        Set(UnifiedExpressions.MouthUpperRight, w[54]);
        Set(UnifiedExpressions.MouthLowerRight, w[54]);
        Set(UnifiedExpressions.NoseSneerLeft, w[55]);
        Set(UnifiedExpressions.NoseSneerRight, w[56]);
        Set(UnifiedExpressions.BrowOuterUpLeft, w[57]);
        Set(UnifiedExpressions.BrowOuterUpRight, w[58]);
        Set(UnifiedExpressions.EyeWideLeft, w[59]);
        Set(UnifiedExpressions.EyeWideRight, w[60]);
        Set(UnifiedExpressions.MouthUpperUpLeft, w[61]);
        Set(UnifiedExpressions.MouthUpperUpRight, w[62]);
        Set(UnifiedExpressions.TongueOut, w[63]);
        Set(UnifiedExpressions.TongueLeft, w[64]);
        Set(UnifiedExpressions.TongueRight, w[65]);
        Set(UnifiedExpressions.TongueUp, w[66]);
        Set(UnifiedExpressions.TongueDown, w[67]);
    }

    private static void Set(UnifiedExpressions expression, float value) =>
        UnifiedTracking.Data.Shapes[(int)expression].Weight = value;

    private static void SetMax(UnifiedExpressions expression, float value)
    {
        ref var target = ref UnifiedTracking.Data.Shapes[(int)expression].Weight;
        target = Math.Max(target, value);
    }
}
