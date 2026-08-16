using VRCFaceTracking;
using VRCFaceTracking.Core.Library;
using System.Runtime.Versioning;

namespace GalaxyXR.VRCFaceTracking;

[SupportedOSPlatform("windows")]
public sealed class GalaxyXRTrackingModule : ExtTrackingModule
{
    private readonly FaceSharedMemoryReader reader = new();
    private readonly float[] weights = new float[68];
    private readonly float[] confidences = new float[3];
    private long nextOpenAttemptTicks;

    public override (bool SupportsEye, bool SupportsExpression) Supported => (true, true);

    public override (bool eyeSuccess, bool expressionSuccess) Initialize(
        bool eyeAvailable,
        bool expressionAvailable)
    {
        ModuleInformation.Name = "Samsung Galaxy XR (GXRP)";
        var opened = reader.Open();
        Status = opened ? ModuleState.Active : ModuleState.Idle;
        nextOpenAttemptTicks = Environment.TickCount64 + 1000;
        return (eyeAvailable, expressionAvailable);
    }

    public override void Update()
    {
        if (reader.TryRead(weights, confidences))
        {
            AndroidXrUnifiedMapper.Apply(weights);
            Status = ModuleState.Active;
        }
        else
        {
            AndroidXrUnifiedMapper.Clear();
            Status = ModuleState.Idle;
            if (Environment.TickCount64 >= nextOpenAttemptTicks)
            {
                reader.Open();
                nextOpenAttemptTicks = Environment.TickCount64 + 1000;
            }
        }
        Thread.Sleep(8);
    }

    public override void Teardown()
    {
        AndroidXrUnifiedMapper.Clear();
        reader.Dispose();
        Status = ModuleState.Uninitialized;
    }
}
