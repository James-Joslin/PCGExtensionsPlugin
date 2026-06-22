// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PCGExtensions : ModuleRules
{
    public PCGExtensions(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new string[] {
				// Allows clean includes across your Public folder structure
			}
        );

        PrivateIncludePaths.AddRange(
            new string[] {
            }
        );

        // Public dependencies are modules whose classes are exposed in your plugin's header files (.h)
        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
                "PCG" // Required for UPCGBlueprintElement, FPCGPoint, etc.
			}
        );

        // Private dependencies are internal implementations used strictly inside your .cpp files
        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
				// Tracing features use standard Engine headers, but keeping clean decoupled layers is best practice
			}
        );

        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
            }
        );
    }
}