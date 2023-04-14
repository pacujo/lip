arch_env = Environment(PREFIX=ARGUMENTS.get('prefix', '/usr/local'))

for dir in [ "src", "components/lip" ]:
    env = arch_env.Clone()
    SConscript(f"{dir}/SConscript",
               exports=["env"],
               variant_dir=f"stage/native/build/{dir}")
