
print_delim()
{
  echo "--------------------------------------------------------------------------------"
}

header()
{
  echo
  print_delim
  echo "$1"
  print_delim
  echo
}


RELEASE=$(cat version.txt)

header "Pushing release $RELEASE"

git tag "$RELEASE"
git push --force origin "$RELEASE"

