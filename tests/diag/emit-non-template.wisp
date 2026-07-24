surface popup {
  anchor = top;
}
surface bar {
  widget btn {
    on_click() = emit(popup);
  }
}
