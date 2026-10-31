; jmp thunks for every d3d12.dll export the proxy does not implement itself.
; p_tbl is filled from System32\d3d12.dll at DLL load (see proxy.cpp, kExports order).
.data
PUBLIC p_tbl
p_tbl QWORD 18 DUP(0)
.code
THUNK MACRO name, idx
name PROC
    jmp qword ptr [p_tbl + 8*idx]
name ENDP
ENDM
THUNK X_Ord99, 0
THUNK X_SetAppCompatStringPointer, 1
THUNK X_D3D12GetDebugInterface, 2
THUNK X_D3D12CoreCreateLayeredDevice, 3
THUNK X_D3D12CoreGetLayeredDeviceSize, 4
THUNK X_D3D12CoreRegisterLayers, 5
THUNK X_D3D12CreateRootSignatureDeserializer, 6
THUNK X_D3D12CreateVersionedRootSignatureDeserializer, 7
THUNK X_D3D12DeviceRemovedExtendedData, 8
THUNK X_D3D12EnableExperimentalFeatures, 9
THUNK X_D3D12GetInterface, 10
THUNK X_D3D12PIXEventsReplaceBlock, 11
THUNK X_D3D12PIXGetThreadInfo, 12
THUNK X_D3D12PIXNotifyWakeFromFenceSignal, 13
THUNK X_D3D12PIXReportCounter, 14
THUNK X_D3D12SerializeRootSignature, 15
THUNK X_D3D12SerializeVersionedRootSignature, 16
THUNK X_GetBehaviorValue, 17
END
