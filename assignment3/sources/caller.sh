cd tests
filenames=`ls`
for entry in $filenames
do
    bash "$entry"
    sleep 1
done
