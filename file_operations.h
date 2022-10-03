
/* This will list all non-directory files in a directory and create a string in the following format:
 * "file1","file2","file3"
 * (It removes extensions of the files)
 * 
 * It will also allocate its own c-string buffer for this so just
 * supply an empty pointer like so:
 *
 * char* output_string;
 * if(list_dir_output(SPIFFS, "/path/to/folder", &output_string)
 * {
 *   // your code...
 *   free(output_string);
 * }
 */
bool list_dir_output(fs::FS& fs, const char* dirname, char** output_buffer)
{
  File root = fs.open(dirname);
  if (!root) {
    return false;
  }
  if (!root.isDirectory()) {
    return false;
  }
  
  std::vector<char*> filenames;
  size_t total = 0;

  // additional length for every file (due to the quotation marks and comma)
  const size_t additive_length = 2 + 1; // "len",

  // Go through all files and store their names in a vector
  File file;
  while (file = root.openNextFile()) {
    if (!file.isDirectory()) {
      size_t len = strlen(file.name());
      char* copy = (char*)malloc((len + 1) * sizeof(char));
      memcpy(copy, file.name(), len + 1);
      total += len + additive_length;
      filenames.push_back(copy);
    }
  }

  char* buf = *output_buffer = (char*)malloc((total + 1) * sizeof(char));
  size_t pos = 0;
  for(char* ptr : filenames)
  {
    size_t plen = strlen(ptr);
    strcpy(buf + pos, "\"");

    // find position of '.' in filename
    int32_t until = plen;

    while(until >= 0 && *(ptr + until) != '.')
      until--;
    if(until == -1)
      until = plen;
    ptr[until] = '\0'; // set NULL-Terminator at position of '.'
    plen -= (plen - until);
    strcpy(buf + pos + 1, ptr); // NULL-Terminator is also copied, continously overwritten till the last one stays
    strcpy(buf + pos + plen + 1, "\",");
    pos += plen + additive_length;
    free(ptr);
  }
  return true;
}

// Returns the size of the file behind that path in bytes
size_t file_size(fs::FS& fs, const char* path)
{
  File file = fs.open(path);
  if (!file) return 0;
  size_t result = file.size();
  file.close();
  return result;
}

// reads a file to the output buffer
// THIS IS A VERY INSECURE FUNCTION
// make sure to appropriately size the "output" buffer using the file_size function
bool read_file(fs::FS& fs, const char* path, char* output) {
  File file = fs.open(path);
  if (!file) {
    return false;
  }

  size_t pos = 0;

  while (file.available()) {
    output[pos++] = (char)file.read();
  }
  output[pos] = '\0';
  file.close();
  return true;
}

// Write to a file.
// Either overwrite or append
bool write_file(fs::FS& fs, const char* path, const char* content, bool append)
{
  File file = fs.open(path, append ? FILE_APPEND : FILE_WRITE);
  if (!file) {
    return false;
  }
  bool result = file.print(content);
  file.close();
  return result;
}
