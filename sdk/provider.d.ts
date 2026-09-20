/** Experimental compatibility revision 0.1; additive fixes keep this revision. */
export interface HttpResponse { status: number; body: string }
export interface HttpOptions { method?: 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE'; headers?: Record<string, string>; body?: string }
export interface Host {
    http(url: string, options?: HttpOptions): Promise<HttpResponse>;
    delay(milliseconds: number): Promise<void>;
}
export type Value = null | boolean | string | number | Value[] | { [key: string]: Value };
export type Operation = (arguments_: Record<string, Value>, host: Host) => Record<string, Value> | Promise<Record<string, Value>>;
export type Source = Record<string, Operation>;
export type SourceFactory = (configuration: Record<string, Value>) => Source;
export interface ItemRef { sourceId: string; itemId: string }
export interface VariantRef extends ItemRef { variantId: string }
export interface Page<T> { items: T[]; cursor: string | null; total: number | null; exhausted: boolean }
