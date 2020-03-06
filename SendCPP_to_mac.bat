md iOS
copy ..\..\..\..\code\cppsource\nav-rcn\netdriver\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\ColliderTest\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\Nav\Include\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\Nav\Source\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\Detour\Include\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\Detour\Source\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\DetourCrowd\Include\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\DetourCrowd\Source\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\Octree\*.* iOS
copy ..\..\..\..\code\cppsource\nav-rcn\system\*.* iOS

pscp -l yaowan1 -pw Yw123456 ./iOS/*.* 192.168.16.80:/Users/yaowan1/Documents/cai-nav/cainav/cainav