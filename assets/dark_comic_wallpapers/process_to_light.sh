FILES=$(find ./ -type f -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.png" -o -iname "*.gif")
for FILENAME in $FILES; do
  echo "Processing file: $FILENAME";
  NEW_FILENAME=${FILENAME//dark/light}
  convert $FILENAME -colorspace RGB -negate ../light_comic_wallpapers/$NEW_FILENAME
done

