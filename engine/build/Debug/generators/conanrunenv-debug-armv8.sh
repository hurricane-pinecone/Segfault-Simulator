script_folder="/Users/ayx106047/dev/gameEngine/build/Debug/generators"
echo "echo Restoring environment" > "$script_folder/deactivate_conanrunenv-debug-armv8.sh"
for v in OPENSSL_MODULES
do
   is_defined="true"
   value=$(printenv $v) || is_defined="" || true
   if [ -n "$value" ] || [ -n "$is_defined" ]
   then
       echo export "$v='$value'" >> "$script_folder/deactivate_conanrunenv-debug-armv8.sh"
   else
       echo unset $v >> "$script_folder/deactivate_conanrunenv-debug-armv8.sh"
   fi
done

export OPENSSL_MODULES="/Users/ayx106047/.conan2/p/b/opens8f71d20017489/p/lib/ossl-modules"