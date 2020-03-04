set PackageName=%1

rem cd ../resource

plink.exe -pw Yw123456 yaowan1@192.168.16.80 "cd ~/Documents/AutoBuild/xcode_dir; find GoodAutoBuild/scripts/ -name '*.sh' -exec chmod +x {} \;; GoodAutoBuild/scripts/function/xcode_build_process.sh develop Debug iOS/ Unity-iPhone ~/Documents/AutoBuild/build %PackageName% -no_checkout"
call upload.bat build/%PackageName%-Debug.ipa
rem plink.exe -pw ss yaowan@192.168.16.107 "echo %1 ^| sh -x /Users/goodjenkins/Documents/ProjectZAutoBuild/GoodAutoBuild_Settings/scripts/renameAndSend_SD.sh %PackageName%-Debug ftwar"

rem plink.exe -pw 600dJenkins goodjenkins@172.16.0.181 rm  -rf /Users/goodjenkins/Documents/ProjectZAutoBuild/ProjectZProject/project/Builds/%PackageName%_iOS
rem plink.exe -pw 600dJenkins goodjenkins@172.16.0.181 mv /Users/goodjenkins/Documents/ProjectZAutoBuild/ProjectZProject/project/Builds/iOS /Users/goodjenkins/Documents/ProjectZAutoBuild/ProjectZProject/project/Builds/%PackageName%_iOS


pause