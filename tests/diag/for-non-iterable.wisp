source c = cpu();
surface bar {
  for x in c {
    cell {
      text = "{x}";
    }
  }
}
