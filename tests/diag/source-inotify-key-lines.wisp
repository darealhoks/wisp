source st = inotify(path="/tmp/state", key="critical", lines=4);
surface bar {
  widget s {
    text = st.value;
  }
}
