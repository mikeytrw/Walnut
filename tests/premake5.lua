project "WalnutRequirementsTests"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   staticruntime "off"

   files { "OptionalVulkanRequirementsTests.cpp" }

   includedirs
   {
      "../Walnut/src",
      "../vendor/imgui",
      "%{IncludeDir.VulkanSDK}",
   }

   targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
   objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

   filter "system:windows"
      systemversion "latest"

   filter "configurations:Debug"
      runtime "Release"
      symbols "On"

   filter "configurations:Release"
      runtime "Release"
      optimize "On"
      symbols "On"

   filter "configurations:Dist"
      runtime "Release"
      optimize "On"
      symbols "Off"
