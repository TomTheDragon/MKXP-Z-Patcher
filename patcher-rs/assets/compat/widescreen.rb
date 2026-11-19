# Added by the MKXP-Z Patcher.
# Widescreen modes for RPG Maker VX / VX Ace, chosen in the F1 menu
# ("Widescreen", saved for this game; off by default):
#   1 "16:9, scene colors at the sides": the window is 16:9, the game keeps its
#     own size in the middle, and the engine fills the sides with the scene's
#     colors (blurred). Nothing in the game changes.
#   2 "16:9 wider map view" (experimental): like 1, but maps are shown in 16:9
#     (e.g. 736x416 instead of 544x416), with more of the map around the
#     player; maps narrower than that are centered
#     - pictures (cut-scenes, overlays) stay in the middle, where the game
#       placed them; black bars cover the sides while one fills the screen
#     - message boxes keep the game's own width, in the middle, with their
#       choices, number input, gold and item windows (VX Ace: also the
#       scrolling text)
# Save files stay compatible with Game.exe. It needs the patcher's engine
# (Graphics.widescreen_mode / Graphics.widescreen_resize), else does nothing.

module MkxpzWidescreen
  ACE = defined?(RGSS_VERSION) ? true : false

  @base_w = Graphics.width   # the game's own screen size
  @base_h = Graphics.height
  @applied = nil             # [screen width, window frame width] last set
  @installed = false
  @running = nil             # the scene whose main is running
  @narrow = 0                # > 0: Graphics.width is the game's own width

  class << self
    attr_accessor :running
    attr_reader :base_w, :base_h
  end

  # 16:9 at the game's height, in whole tiles
  def self.wide_w
    (@base_h * 16.0 / 9 / 32).round * 32
  end

  # 0 off, 1 16:9 window (colored sides), 2 also a wider view on maps
  def self.mode
    return 0 unless Graphics.respond_to?(:widescreen_mode) && wide_w > @base_w
    Graphics.widescreen_mode
  end

  # wider than the game's own screen right now (on the map in widescreen mode)
  def self.active?
    Graphics.width > @base_w
  end

  # how far the game's own screen area sits from the left edge
  def self.dx
    [(Graphics.width - @base_w) / 2, 0].max
  end

  # Graphics.width reads as the game's own width in the block (windows made
  # or placed there get the game's sizes; shift then moves them to the middle)
  def self.narrow
    @narrow += 1
    yield
  ensure
    @narrow -= 1
  end

  def self.narrow?
    @narrow > 0
  end

  # moves a window placed for the game's own screen into the middle; again
  # when the view's width changes, or when the game places it anew (in its
  # own coordinates)
  def self.shift(win)
    return if win.nil? || win.disposed?
    set = win.instance_variable_get(:@mkxpz_x)
    want = dx
    if set.nil? || win.x != set
      win.x += want
    elsif (moved = want - win.instance_variable_get(:@mkxpz_dx)) != 0
      win.x += moved
    end
    win.instance_variable_set(:@mkxpz_x, win.x)
    win.instance_variable_set(:@mkxpz_dx, want)
  end

  def self.current_scene
    ACE ? SceneManager.scene : $scene
  end

  # the window's width at the screen's height (16:9 in the widescreen modes;
  # Fullscreen++'s F6 sizes follow it)
  def self.frame_w
    mode >= 1 ? [wide_w, Graphics.width].max : Graphics.width
  end

  # the game changed its screen size itself
  def self.base_changed(w, h)
    @base_w, @base_h = w, h
    @applied = nil
  end

  def self.tick(scene = nil)
    install unless @installed
    return unless @installed && Graphics.respond_to?(:widescreen_resize)
    scene ||= @running || current_scene
    m = mode
    wide_map = m >= 2 && scene.is_a?(Scene_Map)
    want = [wide_map ? wide_w : @base_w, m >= 1 ? wide_w : @base_w]
    if want != @applied
      Graphics.widescreen_resize(want[0], @base_h, want[1]) if @applied || m >= 1
      @applied = want
    end
    # (left alone in games that never had the wider view: their saves stay
    # untouched)
    @ever_on ||= m >= 2
    map_update(scene) if @ever_on && scene.is_a?(Scene_Map)
  rescue => e
    System.puts("[widescreen] #{e.class}: #{e.message}") rescue nil
  end

  # --- on the map

  def self.map_update(scene)
    w = Graphics.width
    if $game_map && $game_player && $game_map.instance_variable_get(:@mkxpz_view_w) != w
      # the view's width changed since the map's display position was set
      # (new game, loaded save, setting switched): center on the player again
      $game_map.instance_variable_set(:@mkxpz_view_w, w)
      $game_map.mkxpz_view_changed if $game_map.respond_to?(:mkxpz_view_changed)
      $game_player.center($game_player.x, $game_player.y)
      fix_antilag
    end
    fix_message(scene)
  end

  # Anti-lag scripts (e.g. in "Changed") remember in every event how wide the
  # screen was when the event was made, and stop updating events beyond it.
  def self.fix_antilag
    return unless defined?(X_OFFSCREEN_SQUARES)
    value = (Graphics.width + X_OFFSCREEN_SQUARES * 32) * 8
    chars = $game_map.events.values + [$game_player]
    chars.each do |c|
      next unless c.instance_variable_defined?(:@x_offscreen_value)
      c.instance_variable_set(:@x_offscreen_value, value) unless c.instance_variable_get(:@x_offscreen_value).nil?
    end
  end

  def self.fix_message(scene)
    mw = scene.instance_variable_get(:@message_window)
    if mw && !mw.disposed?
      shift(mw)
      shift(mw.instance_variable_get(:@gold_window))
      # (VX places its number input at the message box itself)
      %w[@choice_window @number_window @item_window].each { |n| shift(mw.instance_variable_get(n)) } if ACE
      back = mw.instance_variable_get(:@back_sprite) # the dimmed background
      back.x = mw.x if back && !back.disposed? && back.x != mw.x
    end
    shift(scene.instance_variable_get(:@scroll_text_window)) if ACE
  end

  def self.set_rect(v, x, y, w, h)
    return if v.nil? || v.disposed?
    r = v.rect
    v.rect = Rect.new(x, y, w, h) unless r.x == x && r.y == y && r.width == w && r.height == h
  end

  # viewports, pictures and bars of a Spriteset_Map, every frame
  def self.layout_map(ss)
    v1 = ss.instance_variable_get(:@viewport1)
    v2 = ss.instance_variable_get(:@viewport2)
    v3 = ss.instance_variable_get(:@viewport3)
    w, h = Graphics.width, Graphics.height
    if active?
      mw = $game_map.width * 32
      if mw < w && !$game_map.loop_horizontal?
        set_rect(v1, (w - mw) / 2, 0, mw, h)     # narrower map: centered
      else
        set_rect(v1, 0, 0, w, h)
      end
      set_rect(v2, 0, 0, w, h)
      set_rect(v3, 0, 0, w, h)
      ss.instance_variable_set(:@mkxpz_touched, true)
    elsif ss.instance_variable_get(:@mkxpz_touched)
      # switched off: back to the full screen once, then leave them alone
      [v1, v2, v3].each { |v| set_rect(v, 0, 0, w, h) }
      ss.instance_variable_set(:@mkxpz_touched, false)
    end
    update_bars(ss, v2)
  end

  # black bars beside a picture that fills the game's own screen
  def self.update_bars(ss, viewport)
    bars = ss.instance_variable_get(:@mkxpz_bars)
    side = dx
    opacity = 0
    if side > 0
      (ss.instance_variable_get(:@picture_sprites) || []).each do |s|
        next if s.nil? || s.disposed? || !s.visible || s.opacity <= 0
        b = s.bitmap
        next if b.nil? || b.disposed?
        left = s.x - s.ox * s.zoom_x
        top = s.y - s.oy * s.zoom_y
        next unless left <= side + 1 && top <= 1 &&
                    left + b.width * s.zoom_x >= side + @base_w - 1 &&
                    top + b.height * s.zoom_y >= @base_h - 1
        opacity = s.opacity if s.opacity > opacity
      end
    end
    if opacity == 0
      bars.each { |b| b.visible = false } if bars
      return
    end
    h = Graphics.height
    if bars.nil? || bars[0].bitmap.width != side || bars[0].bitmap.height != h
      bars.each { |b| b.bitmap.dispose; b.dispose } if bars
      bars = 2.times.map do
        b = Sprite.new(viewport)
        b.bitmap = Bitmap.new(side, h)
        b.bitmap.fill_rect(0, 0, side, h, Color.new(0, 0, 0))
        b.z = 9999
        b
      end
      ss.instance_variable_set(:@mkxpz_bars, bars)
    end
    bars[0].x = 0
    bars[1].x = Graphics.width - side
    bars.each { |b| b.visible = true; b.opacity = opacity }
  end

  def self.dispose_bars(ss)
    bars = ss.instance_variable_get(:@mkxpz_bars)
    bars.each { |b| b.bitmap.dispose; b.dispose } if bars
    ss.instance_variable_set(:@mkxpz_bars, nil)
  end

  # screenshots (menu and battle backgrounds, save thumbnails): just the
  # game's own area, as the game expects
  def self.crop(bmp)
    return bmp if bmp.nil? || bmp.disposed? || bmp.width <= @base_w
    out = Bitmap.new(@base_w, bmp.height)
    out.blt(0, 0, bmp, Rect.new((bmp.width - @base_w) / 2, 0, @base_w, bmp.height))
    bmp.dispose
    out
  end

  # --- hooks into the game's classes (prepended once they exist, at the
  # first frame, so they run before the game's own versions)

  module SceneHook
    def main
      MkxpzWidescreen.running = self
      MkxpzWidescreen.tick(self) # before the scene builds its sprites
      super
    ensure
      MkxpzWidescreen.running = nil
    end
  end

  module SpritesetHook
    def initialize(*args)
      super
      MkxpzWidescreen.layout_map(self)
    end

    def update(*args)
      super
      MkxpzWidescreen.layout_map(self)
    end

    def dispose(*args)
      MkxpzWidescreen.dispose_bars(self)
      super
    end
  end

  module PictureHook
    def update(*args)
      super
      # (VX leaves a hidden picture's position alone)
      self.x += MkxpzWidescreen.dx if visible
    end
  end

  # VX Ace message windows: made at the game's own width (Window_Message
  # makes its choice, number, item and gold windows itself)
  module NarrowInitHook
    def initialize(*args)
      MkxpzWidescreen.narrow { super }
    end
  end

  # VX Ace choice list and number input: placed when they open
  module NarrowPlacementHook
    def update_placement(*args)
      r = MkxpzWidescreen.narrow { super }
      @mkxpz_x = nil # placed anew
      MkxpzWidescreen.shift(self)
      r
    end
  end

  # VX Ace: maps narrower than the view don't scroll (the stock script would
  # move them off to the left)
  module AceMapHook
    def scroll_right(distance)
      return super if loop_horizontal? || width >= screen_tile_x
    end
  end

  # VX: the stock scripts have 544 / 17 tiles written into the scrolling
  module VXMapHook
    def mkxpz_view_changed
      @margin_x = (width - Graphics.width / 32) * 256 / 2
    end

    def setup_scroll(*args)
      super
      mkxpz_view_changed if MkxpzWidescreen.active?
    end

    def scroll_right(distance)
      return super if !MkxpzWidescreen.active? || loop_horizontal?
      last_x = @display_x
      @display_x = [[@display_x + distance, (width - Graphics.width / 32) * 256].min, 0].max
      @parallax_x += @display_x - last_x
    end

    def calc_parallax_x(bitmap)
      return super if !MkxpzWidescreen.active? || bitmap.nil? || @parallax_loop_x || loop_horizontal?
      w1 = bitmap.width - Graphics.width
      w2 = @map.width * 32 - Graphics.width
      (w1 <= 0 || w2 <= 0) ? 0 : @parallax_x * w1 / w2 / 8
    end
  end

  module VXPlayerHook
    def mkxpz_center_x
      (Graphics.width / 2 - 16) * 8
    end

    def center(x, y)
      return super unless MkxpzWidescreen.active?
      display_x = x * 256 - mkxpz_center_x
      unless $game_map.loop_horizontal?
        max_x = ($game_map.width - Graphics.width / 32) * 256
        display_x = [0, [display_x, max_x].min].max
      end
      display_y = y * 256 - Game_Player::CENTER_Y
      unless $game_map.loop_vertical?
        max_y = ($game_map.height - Graphics.height / 32) * 256
        display_y = [0, [display_y, max_y].min].max
      end
      $game_map.set_display_pos(display_x, display_y)
    end

    def update_scroll(last_real_x, last_real_y)
      return super unless MkxpzWidescreen.active?
      cx = mkxpz_center_x
      cy = Game_Player::CENTER_Y
      ax1 = $game_map.adjust_x(last_real_x)
      ay1 = $game_map.adjust_y(last_real_y)
      ax2 = $game_map.adjust_x(@real_x)
      ay2 = $game_map.adjust_y(@real_y)
      $game_map.scroll_down(ay2 - ay1) if ay2 > ay1 && ay2 > cy
      $game_map.scroll_left(ax1 - ax2) if ax2 < ax1 && ax2 < cx
      $game_map.scroll_right(ax2 - ax1) if ax2 > ax1 && ax2 > cx
      $game_map.scroll_up(ay1 - ay2) if ay2 < ay1 && ay2 < cy
    end
  end

  # once the game's scripts have defined the classes (at the first frame)
  def self.install
    needed = %w[Scene_Base Scene_Map Spriteset_Map Game_Map Game_Player Sprite_Picture]
    needed << 'SceneManager' if ACE
    return unless needed.all? { |n| Object.const_defined?(n) }
    Scene_Base.prepend(SceneHook)
    Spriteset_Map.prepend(SpritesetHook)
    Sprite_Picture.prepend(PictureHook)
    if ACE
      Game_Map.prepend(AceMapHook)
      %w[Window_Message Window_ScrollText].each do |n|
        Object.const_get(n).prepend(NarrowInitHook) if Object.const_defined?(n)
      end
      %w[Window_ChoiceList Window_NumberInput].each do |n|
        Object.const_get(n).prepend(NarrowPlacementHook) if Object.const_defined?(n)
      end
    else
      Game_Map.prepend(VXMapHook)
      Game_Player.prepend(VXPlayerHook)
    end
    @installed = true
  end
end

# Graphics: aliased, not prepended, since game scripts alias these methods too
# (e.g. Fullscreen++ in "Changed"), and an alias made after a prepend points
# back at the prepended method: the two would call each other without end.
# (Preload scripts run once, not again at an F12 reset.)
class << Graphics
  alias_method :mkxpz_ws_width, :width
  def width
    w = mkxpz_ws_width
    MkxpzWidescreen.narrow? && w > MkxpzWidescreen.base_w ? MkxpzWidescreen.base_w : w
  end

  alias_method :mkxpz_ws_update, :update
  def update(*args)
    MkxpzWidescreen.tick
    mkxpz_ws_update(*args)
  end

  alias_method :mkxpz_ws_resize_screen, :resize_screen
  def resize_screen(w, h)
    r = mkxpz_ws_resize_screen(w, h)
    MkxpzWidescreen.base_changed(w, h)
    r
  end

  alias_method :mkxpz_ws_snap_to_bitmap, :snap_to_bitmap
  def snap_to_bitmap(*args)
    bmp = mkxpz_ws_snap_to_bitmap(*args)
    MkxpzWidescreen.active? ? MkxpzWidescreen.crop(bmp) : bmp
  end
end
