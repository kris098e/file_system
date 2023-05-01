dir = ./tests
for entry in "$dir"/*
do
    bash "tests/$entry"
    sleep 5
done